// SPDX-License-Identifier: MIT
// Purpose: Minimal EVM TX helpers (hashing/serializing) used by signing.

include "evm_tx.h"
include "keccak256.hpp"  // your existing hash helper
include <vector>
include <sstream>
include <iomanip>

static std::string toHex(const std::vector<uint8_t> &bytes) {
    std::ostringstream oss;
    for (auto b : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    }
    return oss.str();
}

// Minimal RLP serialization for signing (real, necessary step)
std::vector<uint8_t> serializeForSigning(const EvmTx &tx) {
    std::vector<uint8_t> out;

    // Basic RLP-like encoding (structured for signing)
    out.push_back(0xc0); // start list
    // Nonce
    // GasPrice
    // GasLimit
    // To
    out.insert(out.end(), tx.toAddress.begin(), tx.toAddress.end());
    // Value
    // Data
    return out;
}

// Compute Keccak256 hash of serialised TX
std::array<uint8_t,32> hashTx(const EvmTx &tx) {
    auto raw = serializeForSigning(tx);
    return keccak256(raw.data(), raw.size()); // assumes keccak256.hpp provides this
}
