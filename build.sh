#!/usr/bin/env bash
set -e

OUTPUT="MedorCoin-Blockchain"
LOGFILE="compile_errors.log"

echo "Compiling MedorCoin backend..."

# All include directories
INCLUDE_PATHS="-I./include -I./src -I./src/evm -I./src/crypto -I./src/net"

# Find ALL .cpp files recursively in the project
ALL_CPP=$(find . -type f -name "*.cpp")

# Run compile
g++ $ALL_CPP $INCLUDE_PATHS \
    -std=c++20 -pthread -O2 \
    -L/usr/lib/x86_64-linux-gnu -lsecp256k1 \
    -o $OUTPUT 2> $LOGFILE

# Check result
if [ $? -eq 0 ]; then
    echo "✔️ Compilation successful! Binary created: $OUTPUT"
    rm -f $LOGFILE
else
    echo "❌ Compilation failed — see $LOGFILE"
fi
