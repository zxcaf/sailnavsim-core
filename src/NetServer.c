/**
 * Copyright (C) 2021 ls4096 <ls4096@8bitbyte.ca>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <proteus/Ocean.h>
#include <proteus/Wave.h>
#include <proteus/Weather.h>

#include "NetServer.h"
#include "ErrLog.h"


#define ERRLOG_ID "NetServer"
#define THREAD_NAME "NetServer"


#define REQ_TYPE_INVALID		(0)
#define REQ_TYPE_GET_WIND		(1)
#define REQ_TYPE_GET_WIND_GUST		(2)
#define REQ_TYPE_GET_OCEAN_CURRENT	(3)
#define REQ_TYPE_GET_SEA_ICE		(4)
#define REQ_TYPE_GET_WAVE_HEIGHT	(5)

static const char* REQ_STR_GET_WIND = "wind";
static const char* REQ_STR_GET_WIND_GUST = "wind_gust";
static const char* REQ_STR_GET_OCEAN_CURRENT = "ocean_current";
static const char* REQ_STR_GET_SEA_ICE = "sea_ice";
static const char* REQ_STR_GET_WAVE_HEIGHT = "wave_height";


#define REQ_MAX_ARG_COUNT (2)

#define REQ_VAL_NONE (0)
#define REQ_VAL_INT (1)
#define REQ_VAL_DOUBLE (2)

static const uint8_t REQ_VALS_NONE[REQ_MAX_ARG_COUNT] = { REQ_VAL_NONE, REQ_VAL_NONE };

static const uint8_t REQ_VALS_LAT_LON[REQ_MAX_ARG_COUNT] = { REQ_VAL_DOUBLE, REQ_VAL_DOUBLE };


typedef union
{
	int i;
	double d;
} ReqValue;


#define INVALID_INTEGER_VALUE (-999)
#define INVALID_DOUBLE_VALUE (-999.0)


static void* netServerThreadMain(void* arg);
static int startListen(int port);

static int handleMessage(int fd, char* reqStr);

static int getReqType(const char* s);
static const uint8_t* getReqExpectedValueTypes(int reqType);
static bool areValuesValidForReqType(int reqType, ReqValue values[REQ_MAX_ARG_COUNT]);

static void populateWindResponse(char* buf, size_t bufSize, proteus_GeoPos* pos, bool gust);
static void populateOceanResponse(char* buf, size_t bufSize, proteus_GeoPos* pos, bool seaIce);
static void populateWaveResponse(char* buf, size_t bufSize, proteus_GeoPos* pos);


static pthread_t _netServerThread;
static int _listenFd = 0;


int NetServer_init(int port)
{
	if (port < 0)
	{
		return -3;
	}

	if (startListen(port) != 0)
	{
		ERRLOG1("Failed to start listening on localhost port %d!", port);
		return -2;
	}

	ERRLOG1("Listening on port %d", port);

	if (pthread_create(&_netServerThread, 0, &netServerThreadMain, 0) != 0)
	{
		ERRLOG("Failed to start net server thread!");
		return -1;
	}

#if defined(_GNU_SOURCE) && defined(__GLIBC__)
	if (0 != pthread_setname_np(_netServerThread, THREAD_NAME))
	{
		ERRLOG1("Couldn't set thread name to %s. Continuing anyway.", THREAD_NAME);
	}
#endif

	return 0;
}


#define MSG_BUF_SIZE (1024)

// Statistics counters
#define COUNTER_ACCEPT		(0)
#define COUNTER_ACCEPT_FAIL	(1)
#define COUNTER_READ		(2)
#define COUNTER_READ_FAIL	(3)
#define COUNTER_DATA_TOO_LONG	(4)
#define COUNTER_MESSAGE		(5)
#define COUNTER_MESSAGE_FAIL	(6)
static unsigned int _counter[COUNTER_MESSAGE_FAIL + 1] = { 0 };

static void* netServerThreadMain(void* arg)
{
	char buf[MSG_BUF_SIZE];

	ERRLOG("Server thread preparing to accept...");

	// NOTE: This is a single-threaded one-request-at-a-time server loop!
	//       Any expectations about its performance and behaviour should therefore take this into account.
	for (;;)
	{
		// Occasionally log statistics counters.
		if ((_counter[COUNTER_ACCEPT] & 0x03ff) == 0)
		{
			ERRLOG7("Stats: accept=%u, accept_fail=%u, read=%u, read_fail=%u, data_too_long=%u, message=%u, message_fail=%u", \
					_counter[COUNTER_ACCEPT], \
					_counter[COUNTER_ACCEPT_FAIL], \
					_counter[COUNTER_READ], \
					_counter[COUNTER_READ_FAIL], \
					_counter[COUNTER_DATA_TOO_LONG], \
					_counter[COUNTER_MESSAGE], \
					_counter[COUNTER_MESSAGE_FAIL]);
		}

		struct sockaddr_in peer;
		socklen_t sl = sizeof(struct sockaddr_in);

		int fd = accept(_listenFd, (struct sockaddr*) &peer, &sl);
		_counter[COUNTER_ACCEPT]++;

		if (fd < 0)
		{
			ERRLOG1("Failed accept! errno=%d", errno);
			_counter[COUNTER_ACCEPT_FAIL]++;

			continue;
		}

		// Number of bytes in read buffer ready for message parsing/processing
		int readyBytes = 0;

		// End of read stream indicator
		bool eos = false;

		// Request loop
		for (;;)
		{
			int rb = 0;

			if (readyBytes == MSG_BUF_SIZE)
			{
				// We've encountered a request message that doesn't fit inside the buffer.
				ERRLOG("Excessive message length!");
				_counter[COUNTER_DATA_TOO_LONG]++;

				break;
			}

			if (!eos)
			{
				// Not the end of the request stream yet, so possibly try to read more data...

				fd_set rfds;
				FD_ZERO(&rfds);
				FD_SET(fd, &rfds);
				struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };

				// Attempt to read from the fd if either of the following conditions is true:
				// 	1. select() tells us the fd has data ready for read.
				// 	2. We have no more request bytes to process for messages in our local buffer.
				if (select(fd + 1, &rfds, 0, 0, &tv) != 0 || readyBytes == 0)
				{
					// Read from the fd.
					rb = read(fd, buf + readyBytes, MSG_BUF_SIZE - readyBytes);
					_counter[COUNTER_READ]++;

					if (rb < 0)
					{
						ERRLOG1("Failed read! errno=%d", errno);
						_counter[COUNTER_READ_FAIL]++;

						break;
					}
					else if (rb == 0)
					{
						// End of stream.
						eos = true;
					}
				}
			}

			// Find the end of the next request message string.
			int i = 0;
			bool foundNewline = false;
			for (const char* s = buf; *s != 0 && i < rb + readyBytes; s++)
			{
				i++;
				if (*s == '\n')
				{
					foundNewline = true;
					break;
				}
			}

			readyBytes += rb;

			if (!foundNewline)
			{
				// Didn't find newline in buffer.
				if (eos)
				{
					// End of request stream, so break out of request loop.
					break;
				}
				else
				{
					// Not end of request stream, so keep trying to read.
					continue;
				}
			}
			foundNewline = false;

			// Replace newline in buffer with null terminator.
			buf[i - 1] = 0;

			// Handle the request message.
			_counter[COUNTER_MESSAGE]++;
			if (handleMessage(fd, buf) != 0)
			{
				ERRLOG("Failed to handle request!");
				_counter[COUNTER_MESSAGE_FAIL]++;

				break;
			}

			// Move start of next message data to start of buffer.
			memmove(buf, buf + i, MSG_BUF_SIZE - i);
			readyBytes -= i;

			if (eos && readyBytes == 0)
			{
				// End of stream, and no more bytes to process.
				break;
			}
		}

		close(fd);
	}

	close(_listenFd);
	_listenFd = 0;

	return 0;
}

static int startListen(int port)
{
	int rc = 0;

	struct sockaddr_in sa;

	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	_listenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (_listenFd < 0)
	{
		ERRLOG1("Failed to open socket! errno=%d", errno);
		return -1;
	}

	rc = bind(_listenFd, (struct sockaddr*) &sa, sizeof(struct sockaddr_in));
	if (rc != 0)
	{
		ERRLOG2("Failed to bind socket! rc=%d errno=%d", rc, errno);
		rc = -2;
		goto done;
	}

	rc = listen(_listenFd, 100);
	if (rc != 0)
	{
		ERRLOG2("Failed to listen on socket! rc=%d errno=%d", rc, errno);
		rc = -3;
		goto done;
	}

done:
	if (rc != 0)
	{
		close(_listenFd);
		_listenFd = 0;
	}

	return rc;
}

static int handleMessage(int fd, char* reqStr)
{
	char* s;
	char* t;

	if ((s = strtok_r(reqStr, ",", &t)) == 0)
	{
		goto fail;
	}

	const int reqType = getReqType(s);
	if (reqType == REQ_TYPE_INVALID)
	{
		goto fail;
	}

	const uint8_t* vals = getReqExpectedValueTypes(reqType);

	ReqValue values[REQ_MAX_ARG_COUNT];

	for (int i = 0; i < REQ_MAX_ARG_COUNT; i++)
	{
		switch (vals[i])
		{
			case REQ_VAL_NONE:
				break;

			case REQ_VAL_INT:
			case REQ_VAL_DOUBLE:
				if ((s = strtok_r(0, ",", &t)) == 0)
				{
					goto fail;
				}

				if (vals[i] == REQ_VAL_INT)
				{
					values[i].i = strtol(s, 0, 10);
				}
				else
				{
					values[i].d = strtod(s, 0);
				}

				break;

			default:
				goto fail;
		}
	}

	if (!areValuesValidForReqType(reqType, values))
	{
		goto fail;
	}


	char buf[MSG_BUF_SIZE];
	proteus_GeoPos pos = { values[0].d, values[1].d };

	switch (reqType)
	{
		case REQ_TYPE_GET_WIND:
			populateWindResponse(buf, MSG_BUF_SIZE, &pos, false);
			break;
		case REQ_TYPE_GET_WIND_GUST:
			populateWindResponse(buf, MSG_BUF_SIZE, &pos, true);
			break;
		case REQ_TYPE_GET_OCEAN_CURRENT:
			populateOceanResponse(buf, MSG_BUF_SIZE, &pos, false);
			break;
		case REQ_TYPE_GET_SEA_ICE:
			populateOceanResponse(buf, MSG_BUF_SIZE, &pos, true);
			break;
		case REQ_TYPE_GET_WAVE_HEIGHT:
			populateWaveResponse(buf, MSG_BUF_SIZE, &pos);
			break;
		default:
			goto fail;
	}

	const int bl = strlen(buf);
	int wt = 0;

	for (;;)
	{
		int wb = write(fd, buf + wt, bl - wt);

		if (wb < 0)
		{
			return -1;
		}
		else
		{
			wt += wb;
		}

		if (wt == bl)
		{
			break;
		}
	}

	return 0;

fail:
	if (write(fd, "error\n", 6) != 6)
	{
		return -1;
	}

	return -1;
}

static int getReqType(const char* s)
{
	if (strncmp(REQ_STR_GET_WIND, s, strlen(s)) == 0)
	{
		return REQ_TYPE_GET_WIND;
	}
	else if (strncmp(REQ_STR_GET_WIND_GUST, s, strlen(s)) == 0)
	{
		return REQ_TYPE_GET_WIND_GUST;
	}
	else if (strncmp(REQ_STR_GET_OCEAN_CURRENT, s, strlen(s)) == 0)
	{
		return REQ_TYPE_GET_OCEAN_CURRENT;
	}
	else if (strncmp(REQ_STR_GET_SEA_ICE, s, strlen(s)) == 0)
	{
		return REQ_TYPE_GET_SEA_ICE;
	}
	else if (strncmp(REQ_STR_GET_WAVE_HEIGHT, s, strlen(s)) == 0)
	{
		return REQ_TYPE_GET_WAVE_HEIGHT;
	}

	return REQ_TYPE_INVALID;
}

static const uint8_t* getReqExpectedValueTypes(int reqType)
{
	switch (reqType)
	{
		case REQ_TYPE_GET_WIND:
		case REQ_TYPE_GET_WIND_GUST:
		case REQ_TYPE_GET_OCEAN_CURRENT:
		case REQ_TYPE_GET_SEA_ICE:
		case REQ_TYPE_GET_WAVE_HEIGHT:
			return REQ_VALS_LAT_LON;
	}

	return REQ_VALS_NONE;
}

static bool areValuesValidForReqType(int reqType, ReqValue values[REQ_MAX_ARG_COUNT])
{
	switch (reqType)
	{
		case REQ_TYPE_GET_WIND:
		case REQ_TYPE_GET_WIND_GUST:
		case REQ_TYPE_GET_OCEAN_CURRENT:
		case REQ_TYPE_GET_SEA_ICE:
		case REQ_TYPE_GET_WAVE_HEIGHT:
		{
			return (values[0].d >= -90.0 && values[0].d <= 90.0 &&
					values[1].d >= -180.0 && values[1].d <= 180.0);
		}
	}

	// All other request types do not use request values and have no restrictions.
	return true;
}

static void populateWindResponse(char* buf, size_t bufSize, proteus_GeoPos* pos, bool gust)
{
	proteus_Weather wx;
	proteus_Weather_get(pos, &wx, true);

	snprintf(buf, bufSize, "%s,%f,%f,%f,%f\n",
			gust ? REQ_STR_GET_WIND_GUST : REQ_STR_GET_WIND,
			pos->lat,
			pos->lon,
			wx.wind.angle,
			gust ? wx.windGust : wx.wind.mag);
}

static void populateOceanResponse(char* buf, size_t bufSize, proteus_GeoPos* pos, bool seaIce)
{
	proteus_OceanData od;
	const bool valid = proteus_Ocean_get(pos, &od);

	if (seaIce)
	{
		snprintf(buf, bufSize, "%s,%f,%f,%f\n",
				REQ_STR_GET_SEA_ICE,
				pos->lat,
				pos->lon,
				valid ? od.ice : INVALID_DOUBLE_VALUE);
	}
	else
	{
		snprintf(buf, bufSize, "%s,%f,%f,%f,%f\n",
				REQ_STR_GET_OCEAN_CURRENT,
				pos->lat,
				pos->lon,
				valid ? od.current.angle : INVALID_DOUBLE_VALUE,
				valid ? od.current.mag : INVALID_DOUBLE_VALUE);
	}
}

static void populateWaveResponse(char* buf, size_t bufSize, proteus_GeoPos* pos)
{
	proteus_WaveData wd;
	const bool valid = proteus_Wave_get(pos, &wd);

	snprintf(buf, bufSize, "%s,%f,%f,%f\n",
			REQ_STR_GET_WAVE_HEIGHT,
			pos->lat,
			pos->lon,
			valid ? wd.waveHeight : INVALID_DOUBLE_VALUE);
}