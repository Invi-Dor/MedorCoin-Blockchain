#ifndef SIGNATURE_H
#define SIGNATURE_H

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

bool sign_hash(
    const unsigned char* hash32,
    const unsigned char* privkey32,
    unsigned char* out_sig65
);

bool verify_hash(
    const unsigned char* hash32,
    const unsigned char* sig65,
    const unsigned char* pubkey_bytes,
    size_t pubkey_len
);

bool recover_pubkey(
    const unsigned char* hash32,
    const unsigned char* sig65,
    unsigned char* out_pubkey
);

#ifdef __cplusplus
}
#endif

#endif  // SIGNATURE_H
