#!/usr/bin/env bash
set -e

OUTPUT="MedorCoin-Blockchain"
LOGFILE="compile_errors.log"

echo "Compiling MedorCoin backend..."

# All include dirs you need
INCLUDES="-I./src -I./src/evm -I./src/crypto -I./src/net"

# Find all .cpp files recursively
ALL_CPP=$(find src -type f -name "*.cpp")

# Actual compile
g++ $ALL_CPP $INCLUDES \
    -std=c++20 -pthread -O2 \
    -L/usr/lib/x86_64-linux-gnu -lsecp256k1 \
    -o $OUTPUT 2> $LOGFILE

if [ $? -eq 0 ]; then
    echo "✔️ Compilation successful! Binary: $OUTPUT"
    rm -f $LOGFILE
else
    echo "❌ Compilation failed — see $LOGFILE"
fi
