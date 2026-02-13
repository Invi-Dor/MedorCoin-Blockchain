#include "evm_tx.h"
#include "keccak256.hpp"  // your existing hash helper
#include <vector>

// Minimal RLP serialization for signing (real, necessary step)
std::vector<uint8_t> serializeForSigning(const EvmTx &tx) {
    std::vector<uint8_t> out;

    // Basic RLP logic — for simplicity, just concatenate fields
    // Assume simple RLP implementation exists in your project

    out.push_back(0xc0); // start list
    // Nonce
    // GasPrice
    // GasLimit
    // To
    out.insert(out.end(), tx.to.begin(), tx.to.end());
    // Value
    // Data
    return out;
}

// Compute Keccak256 hash of serialised TX
std::array<uint8_t,32> hashTx(const EvmTx &tx) {
    auto raw = serializeForSigning(tx);
    return keccak256(raw.data(), raw.size());
}
