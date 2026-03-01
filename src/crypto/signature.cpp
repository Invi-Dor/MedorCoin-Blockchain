#include "signature.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring>

// Sign a 32‑byte hash and return (r, s, v)
std::tuple<std::array<uint8_t,32>, std::array<uint8_t,32>, uint8_t>
signHash(
    const std::array<uint8_t,32> &digest,
    const std::array<uint8_t,32> &privKeyBytes
) {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        throw std::runtime_error("Failed to create secp256k1 context");
    }

    secp256k1_ecdsa_recoverable_signature recsig;
    // Sign with deterministic nonce (RFC6979)
    if (secp256k1_ecdsa_sign_recoverable(
            ctx,
            &recsig,
            digest.data(),
            privKeyBytes.data(),
            secp256k1_nonce_function_default,
            nullptr) != 1) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("secp256k1 signing failed");
    }

    // Serialize to 64 bytes + recid
    unsigned char compact64[64];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        ctx,
        compact64,
        &recid,
        &recsig
    );

    secp256k1_context_destroy(ctx);

    std::array<uint8_t,32> r_bytes;
    std::array<uint8_t,32> s_bytes;
    std::memcpy(r_bytes.data(), compact64, 32);
    std::memcpy(s_bytes.data(), compact64 + 32, 32);
    uint8_t v = static_cast<uint8_t>(recid);

    return std::make_tuple(r_bytes, s_bytes, v);
}
