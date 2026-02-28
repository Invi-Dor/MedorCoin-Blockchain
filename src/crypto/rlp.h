// File: crypto/rlp.h
// SPDX‑License‑Identifier: MIT
// Purpose: Ethereum RLP encoding utilities
#ifndef CRYPTO_RLP_H
#define CRYPTO_RLP_H

#include <cstdint>
#include <vector>

namespace rlp {

// Encode a byte vector as RLP
std::vector<uint8_t> encodeBytes(const std::vector<uint8_t>& input);

// Encode a 64‑bit unsigned as RLP
std::vector<uint8_t> encodeUInt(uint64_t value);

// Encode an RLP list
std::vector<uint8_t> encodeList(const std::vector<std::vector<uint8_t>>& items);

} // namespace rlp

#endif // CRYPTO_RLP_H
