#include "crypto/signature.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring>
#include <stdexcept>
#include <mutex>

namespace crypto {

static secp256k1_context* g_ctx = nullptr;

static void cleanupCtx() {
    if (g_ctx) {
        secp256k1_context_destroy(g_ctx);
        g_ctx = nullptr;
    }
}

static secp256k1_context* getCtx() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        g_ctx = secp256k1_context_create(
            SECP256K1_CONTEXT_SIGN |
            SECP256K1_CONTEXT_VERIFY);
        if (!g_ctx)
            throw std::runtime_error(
                "secp256k1_context_create failed in crypto::signature");
        std::atexit(cleanupCtx);
    });
    return g_ctx;
}

bool verifyHashWithPubkey(
    const unsigned char hash32[32],
    const unsigned char pubkey33[33],
    const unsigned char sig64[64])
{
    if (!hash32 || !pubkey33 || !sig64)
        return false;

    secp256k1_context* ctx = getCtx();

    secp256k1_pubkey pub;
    if (secp256k1_ec_pubkey_parse(ctx, &pub, pubkey33, 33) != 1)
        return false;

    secp256k1_ecdsa_signature sig;
    if (secp256k1_ecdsa_signature_parse_compact(ctx, &sig, sig64) != 1)
        return false;

    // Normalize to low-S to prevent signature malleability (BIP66, EIP-2)
    secp256k1_ecdsa_signature_normalize(ctx, &sig, &sig);

    return secp256k1_ecdsa_verify(ctx, &sig, hash32, &pub) == 1;
}

bool recoverPubkey(
    const unsigned char hash32[32],
    const unsigned char sig64[64],
    int                 recoveryId,
    unsigned char       pubkeyOut33[33])
{
    if (!hash32 || !sig64 || !pubkeyOut33)
        return false;

    // Recovery ID must be 0 or 1. The raw v field from a transaction
    // must be converted to a recovery ID via computeRecoveryId first.
    if (recoveryId < 0 || recoveryId > 1)
        return false;

    secp256k1_context* ctx = getCtx();

    secp256k1_ecdsa_recoverable_signature rsig;
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx, &rsig, sig64, recoveryId) != 1)
        return false;

    // Convert to normalised non-recoverable signature to enforce low-S,
    // then re-parse as recoverable. This prevents malleability during recovery.
    secp256k1_ecdsa_signature normalized;
    secp256k1_ecdsa_recoverable_signature_convert(ctx, &normalized, &rsig);
    secp256k1_ecdsa_signature_normalize(ctx, &normalized, &normalized);

    // Re-parse the normalized sig as recoverable with the same recovery ID
    unsigned char normBytes[64];
    secp256k1_ecdsa_signature_serialize_compact(ctx, normBytes, &normalized);
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx, &rsig, normBytes, recoveryId) != 1)
        return false;

    secp256k1_pubkey pub;
    if (secp256k1_ecdsa_recover(ctx, &pub, &rsig, hash32) != 1)
        return false;

    size_t outLen = 33;
    secp256k1_ec_pubkey_serialize(
        ctx, pubkeyOut33, &outLen, &pub, SECP256K1_EC_COMPRESSED);

    return outLen == 33;
}

int computeRecoveryId(uint64_t v, uint64_t chainId) {
    if (chainId > 0) {
        uint64_t base = 35 + 2 * chainId;
        if (v == base || v == base + 1)
            return static_cast<int>(v - base);
    }
    // Legacy pre-EIP-155
    if (v == 27 || v == 28)
        return static_cast<int>(v - 27);
    return -1;
}

} // namespace crypto
