#include "secp256k1_wrapper.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring>
#include <random>

namespace crypto {

static secp256k1_context* getSigningCtx() {
    static secp256k1_context* ctx =
        secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    return ctx;
}

Secp256k1Keypair generateKeypair() {
    Secp256k1Keypair kp;
    std::random_device rd;
    std::mt19937_64 gen(rd());
    for (auto &b : kp.privkey) b = (uint8_t)(gen() & 0xFF);

    secp256k1_context* ctx = getSigningCtx();
    secp256k1_pubkey pub;
    secp256k1_ec_pubkey_create(ctx, &pub, kp.privkey.data());

    size_t outlen = 65;
    secp256k1_ec_pubkey_serialize(ctx, kp.pubkey_uncompressed.data(), &outlen,
        &pub, SECP256K1_EC_UNCOMPRESSED);

    return kp;
}

std::optional<Secp256k1Signature> signRecoverable(
    const uint8_t hash[32],
    const std::array<uint8_t,32>& privkey
) {
    secp256k1_ecdsa_recoverable_signature sigRec;
    secp256k1_context* ctx = getSigningCtx();

    if (!secp256k1_ecdsa_sign_recoverable(
            ctx,
            &sigRec,
            hash,
            privkey.data(),
            nullptr,
            nullptr
        )) {
        return std::nullopt;
    }

    unsigned char compact[64];
    int recid;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, compact, &recid, &sigRec);

    Secp256k1Signature out;
    std::memcpy(out.r.data(), compact, 32);
    std::memcpy(out.s.data(), compact+32, 32);
    out.recid = recid;
    return out;
}

std::optional<std::array<uint8_t,65>> recoverPubkey(
    const uint8_t hash[32],
    const Secp256k1Signature& sig
) {
    secp256k1_ecdsa_recoverable_signature sigRec;
    secp256k1_context* ctx = getSigningCtx();

    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx,
            &sigRec,
            sig.r.data(),
            sig.recid
        )) {
        return std::nullopt;
    }

    secp256k1_pubkey pub;
    if (!secp256k1_ecdsa_recover(ctx, &pub, &sigRec, hash)) {
        return std::nullopt;
    }

    std::array<uint8_t,65> serialized;
    size_t outlen = 65;
    secp256k1_ec_pubkey_serialize(ctx, serialized.data(), &outlen,
        &pub, SECP256K1_EC_UNCOMPRESSED);

    return serialized;
}

} // namespace crypto
