#include "verify_signature.h"
#include "keccak/keccak.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <vector>
#include <cstring>

// Derive Ethereum address from uncompressed public key (65 bytes)
static std::array<uint8_t,20> pubkeyToAddress(const std::array<uint8_t,65> &pubkey) {
    // Drop the first byte (0x04 — uncompressed prefix)
    const uint8_t *pubBytes = pubkey.data() + 1;
    // Hash 64 bytes (x||y)
    uint8_t hash[32];
    keccak(pubBytes, 64, hash, 32);

    std::array<uint8_t,20> address;
    memcpy(address.data(), hash + 12, 20);
    return address;
}

bool verifyEvmSignature(
    const std::array<uint8_t,32> &hash,
    const std::array<uint8_t,32> &r,
    const std::array<uint8_t,32> &s,
    uint8_t v,
    const std::array<uint8_t,20> &expectedAddress
) {
    // Convert EIP‑155 v to raw recovery id (0 or 1)
    // recid = v − (chainId*2 + 35)
    int recid = v & 0x01;

    secp256k1_context *ctx =
        secp256k1_context_create(SECP256K1_CONTEXT_VERIFY | SECP256K1_CONTEXT_SIGN);

    secp256k1_ecdsa_recoverable_signature sig;
    unsigned char compactSig[64];
    memcpy(compactSig, r.data(), 32);
    memcpy(compactSig + 32, s.data(), 32);

    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(ctx, &sig, compactSig, recid)) {
    secp256k1_context_destroy(ctx);
    return false;
}

secp256k1_pubkey recoveredPub;
if (!secp256k1_ecdsa_recover(ctx, &recoveredPub, &sig, hash.data())) {
    secp256k1_context_destroy(ctx);
    return false;
}

    secp256k1_context_destroy(ctx);

    // Serialize recovered public key (uncompressed)
    std::array<uint8_t,65> pubBytes;
    size_t outLen = 65;
    secp256k1_context *ctx2 = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    secp256k1_ec_pubkey_serialize(ctx2,
                                  pubBytes.data(),
                                  &outLen,
                                  &recoveredPub,
                                  SECP256K1_EC_UNCOMPRESSED);
    secp256k1_context_destroy(ctx2);

    auto addr = pubkeyToAddress(pubBytes);
    return addr == expectedAddress;
}
