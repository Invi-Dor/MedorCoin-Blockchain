#include "crypto/verify_signature.h"
#include "crypto/secp256k1_wrapper.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring>
#include <cstdio>

namespace crypto {

// =============================================================================
// INTERNAL LOGGING
// Returns false and writes a diagnostic to stderr. In production replace
// with your structured logger. Never throws — all functions return bool.
// =============================================================================
static bool fail(const char* fn, const char* reason) {
    std::fprintf(stderr, "[verify_signature] %s: %s\n", fn, reason);
    return false;
}

// =============================================================================
// verifyHashWithPubkey
// Verifies a compact 64-byte ECDSA signature against a known 33-byte
// compressed public key and a 32-byte message hash.
//
// Thread safety: getCtx() uses std::call_once and is fully thread-safe.
// The secp256k1 verify functions are stateless with respect to the context
// and are safe to call concurrently.
// =============================================================================
bool verifyHashWithPubkey(
    const unsigned char hash32[32],
    const unsigned char pubkey33[33],
    const unsigned char sig64[64])
{
    if (!hash32)   return fail("verifyHashWithPubkey", "null hash32");
    if (!pubkey33) return fail("verifyHashWithPubkey", "null pubkey33");
    if (!sig64)    return fail("verifyHashWithPubkey", "null sig64");

    secp256k1_context* ctx = getCtx();

    secp256k1_pubkey pub;
    if (secp256k1_ec_pubkey_parse(ctx, &pub, pubkey33, 33) != 1)
        return fail("verifyHashWithPubkey", "failed to parse public key");

    secp256k1_ecdsa_signature sig;
    if (secp256k1_ecdsa_signature_parse_compact(ctx, &sig, sig64) != 1)
        return fail("verifyHashWithPubkey", "failed to parse compact signature");

    // Normalize to low-S to prevent signature malleability (BIP66, EIP-2).
    // This is necessary here because we are verifying a signature from an
    // external source that may not have enforced low-S.
    secp256k1_ecdsa_signature_normalize(ctx, &sig, &sig);

    if (secp256k1_ecdsa_verify(ctx, &sig, hash32, &pub) != 1)
        return fail("verifyHashWithPubkey", "signature verification failed");

    return true;
}

// =============================================================================
// recoverPubkey
// Recovers the 33-byte compressed public key from a compact 64-byte ECDSA
// signature, a recovery ID, and the 32-byte message hash.
//
// Recovery ID:
//   secp256k1 mathematically allows recovery IDs 0-3, but IDs 2 and 3
//   correspond to curve points with an X coordinate >= the curve order n,
//   which is statistically impossible with standard key generation. We
//   accept 0-3 to be fully spec-compliant, but IDs 2 and 3 will in practice
//   always fail the secp256k1_ecdsa_recover call and return false.
//
// Low-S normalization:
//   secp256k1_ecdsa_recover works correctly with any valid (r, s) pair
//   regardless of S magnitude. We do NOT normalize before recovery because
//   normalization changes S, which changes which public key is recovered.
//   Low-S enforcement belongs at the policy layer (e.g. serialization.cpp),
//   not inside the recovery function.
//
// Thread safety: getCtx() is thread-safe. secp256k1_ecdsa_recover is
// stateless with respect to the context and safe for concurrent calls.
// =============================================================================
bool recoverPubkey(
    const unsigned char hash32[32],
    const unsigned char sig64[64],
    int                 recoveryId,
    unsigned char       pubkeyOut33[33])
{
    if (!hash32)      return fail("recoverPubkey", "null hash32");
    if (!sig64)       return fail("recoverPubkey", "null sig64");
    if (!pubkeyOut33) return fail("recoverPubkey", "null pubkeyOut33");

    // Accept 0-3 per secp256k1 spec. IDs 2 and 3 are valid per spec
    // but will fail recovery in practice with standard signatures.
    if (recoveryId < 0 || recoveryId > 3)
        return fail("recoverPubkey", "recoveryId out of range 0-3");

    secp256k1_context* ctx = getCtx();

    secp256k1_ecdsa_recoverable_signature rsig;
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx, &rsig, sig64, recoveryId) != 1)
        return fail("recoverPubkey",
                    "failed to parse recoverable signature");

    // Recover the public key directly from the recoverable signature.
    // No S normalization — see function comment above.
    secp256k1_pubkey pub;
    if (secp256k1_ecdsa_recover(ctx, &pub, &rsig, hash32) != 1)
        return fail("recoverPubkey", "secp256k1_ecdsa_recover failed");

    // Serialize as compressed 33-byte public key
    size_t outLen = 33;
    if (secp256k1_ec_pubkey_serialize(
            ctx, pubkeyOut33, &outLen, &pub,
            SECP256K1_EC_COMPRESSED) != 1)
        return fail("recoverPubkey", "pubkey serialization failed");

    // Explicit length validation — defensive check
    if (outLen != 33) {
        std::fprintf(stderr,
            "[verify_signature] recoverPubkey: unexpected output length %zu\n",
            outLen);
        return false;
    }

    return true;
}

// =============================================================================
// computeRecoveryId
// Extracts recovery ID (0 or 1) from the raw v field of a signed transaction.
// EIP-155: v = 35 + 2*chainId + recoveryId
// Legacy:  v = 27 + recoveryId
// Returns -1 if v is not valid for the given chainId.
// =============================================================================
int computeRecoveryId(uint64_t v, uint64_t chainId) {
    if (chainId > 0) {
        // Guard against overflow: 35 + 2*chainId must not wrap
        if (chainId <= (0xFFFFFFFFFFFFFFFFULL - 35) / 2) {
            uint64_t base = 35 + 2 * chainId;
            if (v == base || v == base + 1)
                return static_cast<int>(v - base);
        }
    }
    // Legacy pre-EIP-155
    if (v == 27 || v == 28)
        return static_cast<int>(v - 27);
    return -1;
}

} // namespace crypto
