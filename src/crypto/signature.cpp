#include "signature.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring>

namespace crypto {

std::tuple<std::array<uint8_t,32>,std::array<uint8_t,32>,uint8_t>
signHash(
    const std::array<uint8_t,32>& digest,
    const std::array<uint8_t,32>& privKeyBytes
) {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) throw std::runtime_error("secp256k1 context failed");

    secp256k1_ecdsa_recoverable_signature recsig;
    if (secp256k1_ecdsa_sign_recoverable(
            ctx,
            &recsig,
            digest.data(),
            privKeyBytes.data(),
            secp256k1_nonce_function_default,
            nullptr) != 1
    ) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("sign failed");
    }

    unsigned char compact64[64];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        ctx, compact64, &recid, &recsig
    );

    secp256k1_context_destroy(ctx);

    std::array<uint8_t,32> r, s;
    std::memcpy(r.data(), compact64, 32);
    std::memcpy(s.data(), compact64+32, 32);
    return {r, s, static_cast<uint8_t>(recid)};
}

bool verifyHash(
    const std::array<uint8_t,32>& digest,
    const std::array<uint8_t,32>& sig64,
    uint8_t recid,
    const std::array<uint8_t,33>& pubkeyBytes
) {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) return false;

    secp256k1_ecdsa_recoverable_signature recsig;
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx, &recsig, sig64.data(), recid) != 1
    ) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_ecdsa_signature normsig;
    secp256k1_ecdsa_recoverable_signature_convert(ctx, &normsig, &recsig);

    secp256k1_pubkey pubkey;
    if (secp256k1_ec_pubkey_parse(
            ctx, &pubkey, pubkeyBytes.data(), pubkeyBytes.size()) != 1
    ) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    int ok = secp256k1_ecdsa_verify(ctx, &normsig, digest.data(), &pubkey);
    secp256k1_context_destroy(ctx);
    return ok == 1;
}

bool recoverPubkey(
    const std::array<uint8_t,32>& digest,
    const std::array<uint8_t,32>& sig64,
    uint8_t recid,
    std::array<uint8_t,33>& outPubkey
) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_VERIFY | SECP256K1_CONTEXT_SIGN
    );
    if (!ctx) return false;

    secp256k1_ecdsa_recoverable_signature recsig;
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx, &recsig, sig64.data(), recid) != 1
    ) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_pubkey pubkey;
    if (secp256k1_ecdsa_recover(ctx, &pubkey, &recsig, digest.data()) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    size_t outLen = outPubkey.size();
    if (secp256k1_ec_pubkey_serialize(
            ctx, outPubkey.data(), &outLen, &pubkey,
            SECP256K1_EC_COMPRESSED) != 1
    ) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_context_destroy(ctx);
    return true;
}

} // namespace crypto
