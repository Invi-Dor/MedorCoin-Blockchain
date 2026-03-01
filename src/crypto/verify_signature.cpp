include "signature.h"

include <array>
include <cstring>
include <stdexcept>
include <tuple>

include <secp256k1.h>

namespace crypto {

std::tuple<std::array<uint8_t, 32>, std::array<uint8_t, 32>, uint8_t> signHash(
    const std::array<uint8_t, 32>& digest,
    const std::array<uint8_t, 32>& privkey)
{
    // Context for signing
    secp256k1_context ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        throw std::runtime_error("secp256k1 context creation failed");
    }

    secp256k1_ecdsa_recoverable_signature sig;
    // Sign deterministically
    if (!secp256k1_ecdsa_sign_recoverable(ctx,
                                          &sig,
                                          digest.data(),
                                          privkey.data(),
                                          nullptr,
                                          nullptr)) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("Secp256k1 signing failed");
    }

    // Serialize to compact 64-byte (r||s)
    unsigned char compact[64];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, compact, &recid, &sig);

    std::array<uint8_t, 32> r;
    std::array<uint8_t, 32> s;
    std::memcpy(r.data(), compact, 32);
    std::memcpy(s.data(), compact + 32, 32);

    secp256k1_context_destroy(ctx);

    return std::make_tuple(r, s, static_cast<uint8_t>(recid));
}

} // namespace crypto

// File: src/crypto/verify_signature.cpp
include "signature.h"
include <secp256k1.h>
include <secp256k1_recovery.h>
include <cstring>

namespace crypto {

bool verifyHashWithPubkey(
    const unsigned char hash32[32],
    const unsigned char pubkey33[33],
    const unsigned char sig64[64])
{
    // Create context for verification
    secp256k1_context ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) return false;

    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, pubkey33, 33)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_ecdsa_signature sig;
    if (!secp256k1_ecdsa_signature_parse_compact(ctx, &sig, sig64)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    int ok = secp256k1_ecdsa_verify(ctx, &sig, hash32, &pub);
    secp256k1_context_destroy(ctx);
    return ok == 1;
}

} // namespace crypto
