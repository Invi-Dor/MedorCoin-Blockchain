#!/usr/bin/env bash
set -e

echo "Compiling Medorcoin backend..."
g++ \
  -I./src/crypto \
  -I./src/abi \
  -I/usr/include \
  -I/usr/local/include \
  ./src/crypto/signature.cpp \
  ./src/crypto/verify_signature.cpp \
  ./src/crypto/evm_sign.cpp \
  ./src/crypto/keccak256.cpp \
  ./src/crypto/rlp.cpp \
  ./src/abi/encode.cpp \
  -L/usr/lib/x86_64-linux-gnu \
  -lsecp256k1 \
  -o medorcoin_backend

echo "Build succeeded."
