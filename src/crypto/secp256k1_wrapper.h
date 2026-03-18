#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <secp256k1.h>

namespace crypto {

// =============================================================================
// STRUCTURED LOGGER
// Install once at node startup. Receives (level, function, message).
// Level: 0=error, 1=warning, 2=debug. Default discards all messages.
// Thread-safe.
// =============================================================================
using WrapperLogCallback = std::function<void(int, const char*, const char*)>;
void setWrapperLogger(WrapperLogCallback cb);

// =============================================================================
// DATA TYPES
// =============================================================================
struct Secp256k1Keypair {
    std::array<uint8_t, 32> privkey;
    std::array<uint8_t, 33> pubkey_compressed;
    std::array<uint8_t, 65> pubkey_uncompressed;
};

struct Secp256k1Signature {
    std::array<uint8_t, 32> r;
    std::array<uint8_t, 32> s;
    int                     recid = 0;
};

// =============================================================================
// getCtx
// Returns the shared secp256k1 context. Thread-safe via std::call_once.
// On failure logs the error and throws std::runtime_error rather than
// calling std::terminate, allowing the node to handle startup failures
// gracefully.
// =============================================================================
secp256k1_context* getCtx();

// =============================================================================
// generateKeypair
// Generates a secp256k1 keypair using OS CSPRNG with fallbacks.
// Returns both compressed and uncompressed public keys.
// Returns nullopt on failure with a logged reason.
// =============================================================================
std::optional<Secp256k1Keypair> generateKeypair() noexcept;

// =============================================================================
// signRecoverable
// Signs a 32-byte hash. Buffer size enforced at compile time via std::span.
// Returns nullopt on failure with a logged reason.
// recid is always 0 or 1 for standard secp256k1 signatures.
// =============================================================================
std::optional<Secp256k1Signature> signRecoverable(
    std::span<const uint8_t, 32> hash32,
    std::span<const uint8_t, 32> privkey) noexcept;

// =============================================================================
// recoverPubkeyUncompressed
// Recovers the 65-byte uncompressed public key from a recoverable signature.
// recoveryId must be 0-3 per secp256k1 spec.
// =============================================================================
std::optional<std::array<uint8_t, 65>> recoverPubkeyUncompressed(
    std::span<const uint8_t, 32> hash32,
    const Secp256k1Signature&    sig) noexcept;

// =============================================================================
// recoverPubkeyCompressed
// Recovers the 33-byte compressed public key from a recoverable signature.
// recoveryId must be 0-3 per secp256k1 spec.
// =============================================================================
std::optional<std::array<uint8_t, 33>> recoverPubkeyCompressed(
    std::span<const uint8_t, 32> hash32,
    const Secp256k1Signature&    sig) noexcept;

} // namespace crypto
