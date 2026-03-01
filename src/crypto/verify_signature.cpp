#include "signature.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring>

namespace crypto {

bool verifyHashWithPubkey(
    const unsigned char hash32[32],
    const unsigned char pubkey33[33],
    const unsigned char sig64[64])
{
    secp256k1_context* ctx =
        secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);

    if (!ctx) {
        return false;
    }

    secp256k1_pubkey pub;
    if (secp256k1_ec_pubkey_parse(ctx, &pub, pubkey33, 33) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    secp256k1_ecdsa_signature sig;
    if (secp256k1_ecdsa_signature_parse_compact(ctx, &sig, sig64) != 1) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    int ok = secp256k1_ecdsa_verify(ctx, &sig, hash32, &pub);

    secp256k1_context_destroy(ctx);
    return ok == 1;
}

} // namespace crypto
