#include "signature.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring>

bool recover_pubkey(
    const unsigned char* hash32,
    const unsigned char* sig65,
    unsigned char* out_pubkey
) {
    if (!hash32 || !sig65 || !out_pubkey) return false;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) return false;

    secp256k1_ecdsa_recoverable_signature recsig;
    // parse the recoverable signature (64 bytes + recid)
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx,
            &recsig,
            sig65,
            (int)sig65[64]) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // now recover the public key from the signature and the hash
    secp256k1_pubkey pubkey;
    if (secp256k1_ecdsa_recover(ctx, &pubkey, &recsig, hash32) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // serialize the recovered public key into compressed (33‑byte) format
    size_t outlen = 33;
    if (secp256k1_ec_pubkey_serialize(
            ctx,
            out_pubkey,
            &outlen,
            &pubkey,
            SECP256K1_EC_COMPRESSED) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_context_destroy(ctx);
    return true;
}
