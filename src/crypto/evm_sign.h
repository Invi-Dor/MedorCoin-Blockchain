// File: crypto/evm_sign.h
// SPDX‑License‑Identifier: MIT

#ifndef CRYPTO_EVM_SIGN_H
#define CRYPTO_EVM_SIGN_H

#include <string>
#include <vector>
#include <cstdint>

struct EvmTx {
    uint64_t nonce;
    uint64_t maxPriorityFeePerGas;
    uint64_t maxFeePerGas;
    uint64_t gasLimit;
    std::vector<uint8_t> to;     // 20 bytes
    std::vector<uint8_t> value;  // arbitrary
    std::vector<uint8_t> data;   // calldata
    uint64_t chainId;
    uint64_t v;
    std::vector<uint8_t> r;
    std::vector<uint8_t> s;
};

// Sign tx, return RLP‑encoded signed tx
std::vector<uint8_t> signEvmTransaction(EvmTx& tx, const std::array<uint8_t,32>& privkey);

#endif // CRYPTO_EVM_SIGN_H
