#pragma once

#include <array>
#include <cstdint>
#include <string>

/**
 * Verify EVM transaction signature:
 * - Reconstruct the public key from (r, s, v) and hash
 * - Compute address (last 20 bytes of keccak(publicKey))
 * - Compare with expected fromAddress
 *
 * @param hash      32‑byte Keccak hash used in signing
 * @param r         32‑byte r signature component
 * @param s         32‑byte s signature component
 * @param v         EIP‑155 v value
 * @param expectedAddress 20‑byte expected wallet address
 * @return true if signature is valid and matches expectedAddress
 */
bool verifyEvmSignature(
    const std::array<uint8_t,32> &hash,
    const std::array<uint8_t,32> &r,
    const std::array<uint8_t,32> &s,
    uint8_t v,
    const std::array<uint8_t,20> &expectedAddress
);
