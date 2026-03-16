#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <tuple>

/**
 * signature
 *
 * High-level ECDSA signing interface for Ethereum-format transactions
 * and messages. Wraps secp256k1_wrapper with key loading and secure
 * memory handling.
 *
 * Design guarantees:
 *  - Private key material is zeroed from all stack copies immediately
 *    after use, regardless of whether signing succeeds or throws.
 *  - digest is validated as exactly 32 bytes at the call site via
 *    static_assert so misuse is a compile-time error.
 *  - loadPrivkeyHex validates every hex character before populating
 *    the key array.
 *  - All functions throw std::runtime_error on failure; callers must
 *    wrap calls in try/catch for production error handling.
 */

// Sign a 32-byte digest using a private key loaded from a hex key file.
// The key file must contain exactly one non-comment line of 64 hex chars.
// Returns (r[32], s[32], recid) where recid is 0 or 1.
// Throws std::runtime_error on any failure.
std::tuple<std::array<uint8_t, 32>,
           std::array<uint8_t, 32>,
           uint8_t>
signHash(const std::array<uint8_t, 32> &digest,
         const std::string             &privKeyPath);

// Sign a 32-byte digest using a private key passed directly.
// The privkey parameter is taken by value so the caller's copy is
// unaffected; the local copy is zeroed before the function returns.
// Throws std::runtime_error on any failure.
std::tuple<std::array<uint8_t, 32>,
           std::array<uint8_t, 32>,
           uint8_t>
signHashWithKey(const std::array<uint8_t, 32> &digest,
                std::array<uint8_t, 32>         privkey);
