#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Keccak-f[1600] permutation on a 25-lane state (in-place).
// State is 25 x 64-bit words (1600 bits).
void keccakf(uint64_t st[25]);

// Generic Keccak sponge hash.
// - in:     input message (can be NULL if inlen == 0).
// - inlen:  input length in bytes.
// - out:    output buffer (must not be NULL).
// - outlen: digest length in bytes. For Keccak-256 use 32.
// Returns 1 on success, 0 on invalid arguments.
int keccak(const uint8_t* in, size_t inlen, uint8_t* out, size_t outlen);

// Convenience wrapper for Keccak-256 (32-byte output).
// Returns 1 on success, 0 on invalid arguments.
static inline int keccak_256(const uint8_t* in, size_t inlen, uint8_t out32[32]) {
    return keccak(in, inlen, out32, 32);
}

#ifdef __cplusplus
}
#endif
