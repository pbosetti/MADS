#!/bin/sh
#   _           _ _     _       _     
#  | |__  _   _(_) | __| |  ___| |__  
#  | '_ \| | | | | |/ _` | / __| '_ \ 
#  | |_) | |_| | | | (_| |_\__ \ | | |
#  |_.__/ \__,_|_|_|\__,_(_)___/_| |_|
#
# Python wrapper builder, for development purposes only

if [ "$1" = "-h" ]
then
  echo "Usage: ./build.sh [-b] [-h]"
  echo "Generates the SWIG wrapper code"
  echo "Options:"
  echo "  -b  Also build the extension module"
  echo "  -h  Display this help message"
  exit 0
fi

# This only generates the wrapper code
echo "Generating the SWIG wrapper"
swig -c++ -python -o agent.cpp agent.swg

# If the -b flag is present, also build the extension module
for arg in "$@"
do
  if [ "$arg" = "-b" ]
  then
    # Do something here
    echo "Compiling the extension module"
    clang++ -fPIC -std=c++17 -O3 $(python3-config --cflags) $(python3-config --ldflags) -I.. -I../../products/include -I../../build/_deps/nlohmann_json-src/include -I../../build/_deps/tomlplusplus-src/include -L../../products/lib -L$(python3-config --prefix)/lib -lpython3.12 -lsnappy -lzmq -lzmqpp-static --shared agent.cpp -o _Mads.so
  fi
done