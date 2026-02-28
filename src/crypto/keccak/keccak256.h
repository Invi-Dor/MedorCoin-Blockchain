// File: crypto/keccak256.h
// SPDX‑License‑Identifier: MIT
// Purpose: Keccak‑256 hashing (Ethereum/MedorCoin compatible).

#ifndef CRYPTO_KECCAK256_H
#define CRYPTO_KECCAK256_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace crypto {

// Compute Keccak‑256 hash of arbitrary data.
std::vector<uint8_t> keccak256(const uint8_t* data, size_t len);

// Convenience wrapper for std::vector input.
std::vector<uint8_t> keccak256(const std::vector<uint8_t>& input);

} // namespace crypto

#endif // CRYPTO_KECCAK256_H
