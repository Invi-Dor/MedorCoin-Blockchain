#include "signature.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring>

bool sign_hash(
    const unsigned char* hash32,
    const unsigned char* privkey32,
    unsigned char* out_sig65
) {
    if (!hash32 || !privkey32 || !out_sig65) return false;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) return false;

    secp256k1_ecdsa_recoverable_signature recsig;
    if (secp256k1_ecdsa_sign_recoverable(
            ctx,
            &recsig,
            hash32,
            privkey32,
            secp256k1_nonce_function_default,
            NULL) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    int recid = 0;
    unsigned char compact64[64];
    if (secp256k1_ecdsa_recoverable_signature_serialize_compact(
            ctx, compact64, &recid, &recsig) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::memcpy(out_sig65, compact64, 64);
    out_sig65[64] = static_cast<unsigned char>(recid);

    secp256k1_context_destroy(ctx);
    return true;
}

bool verify_hash(
    const unsigned char* hash32,
    const unsigned char* sig65,
    const unsigned char* pubkey_bytes,
    size_t pubkey_len
) {
    if (!hash32 || !sig65 || !pubkey_bytes) return false;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) return false;

    secp256k1_pubkey pubkey;
    if (secp256k1_ec_pubkey_parse(ctx, &pubkey, pubkey_bytes, pubkey_len) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_ecdsa_recoverable_signature recsig;
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx, &recsig,
            sig65,
            (int)sig65[64]) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_ecdsa_signature normsig;
    secp256k1_ecdsa_recoverable_signature_convert(ctx, &normsig, &recsig);

    int ok = secp256k1_ecdsa_verify(ctx, &normsig, hash32, &pubkey);
    secp256k1_context_destroy(ctx);
    return ok == 1;
}

bool recover_pubkey(
    const unsigned char* hash32,
    const unsigned char* sig65,
    unsigned char* out_pubkey
) {
    if (!hash32 || !sig65 || !out_pubkey) return false;

    int recid = (int)sig65[64];
    if (recid < 0 || recid > 3) return false;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) return false;

    secp256k1_ecdsa_recoverable_signature recsig;
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx, &recsig, sig65, recid) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_pubkey pubkey;
    if (secp256k1_ecdsa_recover(ctx, &pubkey, &recsig, hash32) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    size_t outlen = 33;
    if (secp256k1_ec_pubkey_serialize(
            ctx, out_pubkey, &outlen, &pubkey,
            SECP256K1_EC_COMPRESSED) != 1
    ) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_context_destroy(ctx);
    return true;
}
