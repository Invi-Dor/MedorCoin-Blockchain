// File: src/crypto/signature.h

#ifndef SIGNATURE_H
#define SIGNATURE_H

#include <secp256k1.h>
#include <secp256k1_recovery.h>

/**
 * Signs a 32‑byte hash using a 32‑byte private key and produces
 * a 65‑byte recoverable ECDSA signature.
 *
 * @param hash32      Pointer to 32‑byte message hash.
 * @param privkey32   Pointer to 32‑byte private key.
 * @param out_sig65   Output buffer (65 bytes) for signature + recovery id.
 * @return            true on success, false on failure.
 */
bool sign_hash(
    const unsigned char* hash32,
    const unsigned char* privkey32,
    unsigned char* out_sig65
);

/**
 * Verifies a 65‑byte recoverable ECDSA signature against
 * a 32‑byte hash and a public key.
 *
 * @param hash32        Pointer to 32‑byte message hash.
 * @param sig65         Pointer to 65‑byte recoverable signature.
 * @param pubkey_bytes  Pointer to public key bytes (33 or 65 bytes).
 * @param pubkey_len    Length of public key in bytes.
 * @return              true if signature is valid, false otherwise.
 */
bool verify_hash(
    const unsigned char* hash32,
    const unsigned char* sig65,
    const unsigned char* pubkey_bytes,
    size_t pubkey_len
);

/**
 * Recovers a public key from a 65‑byte recoverable signature and a 32‑byte hash.
 *
 * @param hash32        Pointer to 32‑byte message hash.
 * @param sig65         Pointer to 65‑byte recoverable signature.
 * @param out_pubkey    Buffer to receive serialized public key (33 bytes).
 * @return              true on success, false on failure.
 */
bool recover_pubkey(
    const unsigned char* hash32,
    const unsigned char* sig65,
    unsigned char* out_pubkey
);

#endif // SIGNATURE_H
