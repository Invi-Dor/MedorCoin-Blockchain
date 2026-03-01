#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring> // for std::memcpy

// Sign a 32-byte hash and produce a 65-byte recoverable signature.
// hash32: 32-byte message hash
// privkey32: 32-byte private key
// out_sig65: output buffer with 65 bytes (64 for signature + 1 for recid)
bool sign_hash(
    const unsigned char* hash32,
    const unsigned char* privkey32,
    unsigned char* out_sig65 // 65 bytes: 64 + recovery id
) {
    if (!hash32 || !privkey32 || !out_sig65) {
        return false;
    }

    // Create a signing context
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        return false;
    }

    // Recoverable signature object
    secp256k1_ecdsa_recoverable_signature sig;

    // Sign the hash with RFC6979-deterministic nonce (default nonce function)
    int ret = secp256k1_ecdsa_sign_recoverable(
        ctx,
        &sig,
        hash32,
        privkey32,
        secp256k1_nonce_function_default,
        NULL
    );
    if (!ret) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Serialize to 64-byte compact form and obtain the recovery id (0..3)
    int recid = 0;
    unsigned char compact[64];
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, compact, &recid, &sig);

    // Copy 64-byte signature
    std::memcpy(out_sig65, compact, 64);
    // Store recovery id in the 65th byte
    out_sig65[64] = static_cast<unsigned char>(recid);

    // Cleanup
    secp256k1_context_destroy(ctx);
    return true;
}
