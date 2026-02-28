// File: src/crypto/signature.h

#ifndef SIGNATURE_H
#define SIGNATURE_H

#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstddef>  // for size_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sign a 32‑byte hash using a 32‑byte private key.
 * Produces 65‑byte recoverable ECDSA signature (64 sig + 1 recid).
 */
bool sign_hash(
    const unsigned char* hash32,
    const unsigned char* privkey32,
    unsigned char* out_sig65
);

/**
 * Verify a 65‑byte recoverable signature against a 32‑byte hash
 * and a serialized public key.
 */
bool verify_hash(
    const unsigned char* hash32,
    const unsigned char* sig65,
    const unsigned char* pubkey_bytes,
    size_t pubkey_len
);

/**
 * Recover the compressed public key (33 bytes) from a 65‑byte signature
 * and a 32‑byte message hash.
 */
bool recover_pubkey(
    const unsigned char* hash32,
    const unsigned char* sig65,
    unsigned char* out_pubkey
);

#ifdef __cplusplus
}
#endif

#endif // SIGNATURE_H
