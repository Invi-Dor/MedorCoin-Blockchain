#pragma once

#include <array>
#include <cstdint>
#include <optional>

/**
 * secp256k1_wrapper
 *
 * Production wrapper around libsecp256k1 for Ethereum-compatible signing,
 * verification, and public key recovery.
 *
 * Design guarantees:
 *  - CSPRNG uses /dev/urandom on POSIX and BCryptGenRandom on Windows so
 *    the library is portable across both platforms.
 *  - The secp256k1 context is created once and shared for the process
 *    lifetime; it is never destroyed and is safe for concurrent reads.
 *  - Every function validates its inputs and returns std::nullopt or false
 *    rather than invoking undefined behaviour on bad input.
 *  - No function throws. All failures are communicated via return values.
 *  - getCtx() is exposed in the header so verify_signature.cpp can reuse
 *    the shared context without creating its own.
 */
namespace crypto {

struct Secp256k1Keypair {
    std::array<uint8_t, 32> privkey{};
    std::array<uint8_t, 65> pubkey_uncompressed{};
};

struct Secp256k1Signature {
    std::array<uint8_t, 32> r{};
    std::array<uint8_t, 32> s{};
    int                     recid = 0;
};

// Returns the process-wide secp256k1 context.
// Exposed publicly so sibling translation units can share it.
struct secp256k1_context_struct *getCtx() noexcept;

// Generate a keypair using a CSPRNG.
// Returns std::nullopt if the platform RNG is unavailable or if a valid
// key cannot be produced in 32 attempts (astronomically rare).
std::optional<Secp256k1Keypair> generateKeypair() noexcept;

// Sign a 32-byte hash and return a recoverable signature.
// Returns std::nullopt if hash is null, privkey is invalid, or signing fails.
std::optional<Secp256k1Signature> signRecoverable(
    const uint8_t               hash32[32],
    const std::array<uint8_t, 32> &privkey) noexcept;

// Recover the uncompressed public key (65 bytes) from a recoverable signature.
// Returns std::nullopt on any failure.
std::optional<std::array<uint8_t, 65>> recoverPubkey(
    const uint8_t              hash32[32],
    const Secp256k1Signature  &sig) noexcept;

// Verify a compact 64-byte signature against an uncompressed (65-byte) or
// compressed (33-byte) public key. Applies low-S normalisation (EIP-2).
bool verifySignature(const uint8_t hash32[32],
                     const uint8_t *pubkeyBytes,
                     size_t         pubkeyLen,
                     const uint8_t  sig64[64]) noexcept;

} // namespace crypto
