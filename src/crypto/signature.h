#pragma once

#include <array>
#include <string>
#include <tuple>
#include <cstdint>

/**
 * @brief Sign a 32‑byte message hash using a secp256k1 private key.
 * 
 * The digest must be exactly 32 bytes (Keccak‑256 output).
 * The private key file must be a PEM encoded secp256k1 key.
 * 
 * @param digest The 32‑byte hash to sign
 * @param privKeyPath Filesystem path to the private key PEM
 * @return A tuple of (r, s, v)
 *         where:
 *         - r: 32‑byte signature component
 *         - s: 32‑byte signature component
 *         - v: recovery id (to be calculated separately)
 */
std::tuple<std::array<uint8_t,32>, std::array<uint8_t,32>, uint8_t>
signHash(const std::array<uint8_t,32> &digest,
         const std::string &privKeyPath);
