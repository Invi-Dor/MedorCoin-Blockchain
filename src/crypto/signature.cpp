// File: src/crypto/signature.cpp

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

    // Create a verify-capable context
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) return false;

    // Parse the compact recoverable signature (64 bytes + recovery id)
    secp256k1_ecdsa_recoverable_signature recsig;
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx,
            &recsig,
            sig65,
            (int)sig65[64]) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Recover the public key from the signature and the hash
    secp256k1_pubkey pubkey;
    if (secp256k1_ecdsa_recover(ctx, &pubkey, &recsig, hash32) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Serialize recovered public key in compressed form (33 bytes)
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
