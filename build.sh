#!/usr/bin/env bash
set -e

OUTPUT="MedorCoin-Blockchain"
LOGFILE="compile_errors.log"

echo "Compiling MedorCoin backend..."

# Include directories — add any other include paths if needed
INCLUDES="-I./include -I./src"

# Recursively find all .cpp files in the project
ALL_CPP_FILES=$(find . -type f -name "*.cpp")

# Compile all .cpp files
g++ $ALL_CPP_FILES $INCLUDES \
  -std=c++20 -pthread -O2 \
  -L/usr/lib/x86_64-linux-gnu -lsecp256k1 \
  -o $OUTPUT 2> $LOGFILE

if [ $? -eq 0 ]; then
  echo "Compilation successful! Binary created: $OUTPUT"
  rm -f $LOGFILE
else
  echo "Compilation failed. Check $LOGFILE for details."
fi
