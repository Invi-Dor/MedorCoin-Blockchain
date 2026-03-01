#include "signature.h"
#include <array>
#include <tuple>
#include <stdexcept>
#include <secp256k1.h>

namespace crypto {

// Sign a 32-byte hash with a secp256k1 private key
std::tuple<std::array<uint8_t,32>, std::array<uint8_t,32>, uint8_t>
signHash(const std::array<uint8_t,32> &digest, const std::array<uint8_t,32> &privkey) {

    // Recoverable signing with secp256k1
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(ctx, &sig, digest.data(), privkey.data(), nullptr, nullptr)) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("Secp256k1 recoverable signing failed");
    }

    std::array<uint8_t, 32> r{};
    std::array<uint8_t, 32> s{};
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, r.data(), &recid, &sig);
    s = r; // depending on your wrapper; adjust if needed

    secp256k1_context_destroy(ctx);
    return std::make_tuple(r, s, static_cast<uint8_t>(recid));
}

} // namespace crypto
