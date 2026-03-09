#!/usr/bin/env bash
set -e

OUTPUT="MedorCoin-Blockchain"
LOGFILE="compile_errors.log"

echo "Compiling MedorCoin backend..."

# Include directories
INCLUDES="-I./include -I./src -I./src/crypto -I./src/abi -I/usr/include -I/usr/local/include"

# Find all .cpp files
SRC_FILES=$(find src -name "*.cpp")
ROOT_FILES=$(find . -maxdepth 1 -name "*.cpp")

# Compile with secp256k1 library
g++ $SRC_FILES $ROOT_FILES $INCLUDES -std=c++20 -pthread -O2 -L/usr/lib/x86_64-linux-gnu -lsecp256k1 -o $OUTPUT 2> $LOGFILE

# Check compilation
if [ $? -eq 0 ]; then
    echo "Compilation successful! Binary created: $OUTPUT"
    rm -f $LOGFILE
else
    echo "Compilation failed. Check $LOGFILE for details."
fi
