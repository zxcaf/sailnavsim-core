# SailNavSim core simulator

A global sailing navigation simulator, using real-world geographic information and weather/ocean data (as seen here: https://8bitbyte.ca/sailnavsim/).

## Dependencies

- POSIX threads (pthread) library, with headers
- SQLite3 library, with headers

### Build tools

- make
- gcc
- Rust toolchain (rustc, cargo, etc.)

### Tested build/run environments

- Ubuntu 20.04, x86-64
- Debian 10 (Buster), x86-64

## How to build

`make sailnavsim`

## How to run

Create the named pipe to be able to send the simulator commands:

`mkfifo cmds`

Basic run:

`LD_LIBRARY_PATH=./libproteus ./sailnavsim`

With optional TCP server listening on localhost:$PORT (for weather data, live boat info, etc.):

`LD_LIBRARY_PATH=./libproteus ./sailnavsim --netport $PORT`

Performance test run:

`LD_LIBRARY_PATH=./libproteus ./sailnavsim --perf`

### Add a boat

`echo "TestBoat,add,44.0,-63.0,0,0" > cmds`

### Set a course and start the boat

`echo "TestBoat,course,90" > cmds`

`echo "TestBoat,start" > cmds`

### Stop and remove the boat

`echo "TestBoat,stop" > cmds`

`echo "TestBoat,remove" > cmds`

## Build and run tests

`make tests`

`./run_tests.sh`
