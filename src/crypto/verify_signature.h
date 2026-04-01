#pragma once

#include <cstdint>

namespace crypto {

// =============================================================================
// verifyHashWithPubkey
// Verifies a compact 64-byte ECDSA signature against a 33-byte compressed
// public key and a 32-byte message hash.
//
// Parameters:
//   hash32   - 32-byte message hash (SHA-256 or keccak256)
//   pubkey33 - 33-byte compressed secp256k1 public key
//   sig64    - 64-byte compact ECDSA signature (r || s)
//
// Returns true if signature is valid. Returns false on any failure.
// Does not throw. Safe to call from multiple threads simultaneously.
// =============================================================================
bool verifyHashWithPubkey(
    const unsigned char hash32[32],
    const unsigned char pubkey33[33],
    const unsigned char sig64[64]);

// =============================================================================
// recoverPubkey
// Recovers the 33-byte compressed public key from a compact 64-byte ECDSA
// signature, a recovery ID, and the 32-byte message hash that was signed.
//
// Use this during transaction deserialization to recover the sender public
// key from (r, s, v) and derive the sender address without storing the
// public key explicitly in the transaction.
//
// Parameters:
//   hash32      - 32-byte message hash
//   sig64       - 64-byte compact signature (r || s)
//   recoveryId  - must be 0 or 1 (use computeRecoveryId to extract from v)
//   pubkeyOut33 - output buffer receiving the recovered 33-byte compressed key
//
// Returns true on success. Returns false on any failure including bad inputs.
// Does not throw. Safe to call from multiple threads simultaneously.
// =============================================================================
bool recoverPubkey(
    const unsigned char hash32[32],
    const unsigned char sig64[64],
    int                 recoveryId,
    unsigned char       pubkeyOut33[33]);

// =============================================================================
// computeRecoveryId
// Extracts the secp256k1 recovery ID (0 or 1) from the raw v field of a
// signed transaction.
//
// EIP-155 (replay protection): v = 35 + 2 * chainId + recoveryId
// Legacy (pre-EIP-155):        v = 27 + recoveryId
//
// Parameters:
//   v       - raw v value from the signed transaction
//   chainId - network chain ID (pass 0 for legacy transactions)
//
// Returns 0 or 1 on success.
// Returns -1 if v is not valid for the given chainId.
// =============================================================================
int computeRecoveryId(uint64_t v, uint64_t chainId);

} // namespace crypto
