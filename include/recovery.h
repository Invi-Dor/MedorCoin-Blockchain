#pragma once

#include <array>
#include <cstdint>

/**
 * Recover the raw ECDSA recovery ID (0 or 1) from a signature.
 * 
 * @param hash      32‑byte Keccak hash of the transaction
 * @param r         32‑byte r part of signature
 * @param s         32‑byte s part of signature
 * @param outPubkey 65‑byte buffer to receive the recovered uncompressed public key
 * @return recovery ID (0 or 1)
 */
int findRecoveryId(
    const std::array<uint8_t,32> &hash,
    const std::array<uint8_t,32> &r,
    const std::array<uint8_t,32> &s,
    std::array<uint8_t,65> &outPubkey
);
