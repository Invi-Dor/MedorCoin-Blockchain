// From tiny‑keccak (MIT licensed)
// This is the core Keccak‑256 implementation for Ethereum

#include "keccak/keccak.h"

#define ROL64(a, offset) (((a) << (offset)) | ((a) >> (64 - (offset))))

// Keccak round constants
static const uint64_t keccakf_rndc[24] = {
    (uint64_t)0x0000000000000001ULL, (uint64_t)0x0000000000008082ULL,
    (uint64_t)0x800000000000808aULL, (uint64_t)0x8000000080008000ULL,
    (uint64_t)0x000000000000808bULL, (uint64_t)0x0000000080000001ULL,
    (uint64_t)0x8000000080008081ULL, (uint64_t)0x8000000000008009ULL,
    (uint64_t)0x000000000000008aULL, (uint64_t)0x0000000000000088ULL,
    (uint64_t)0x0000000080008009ULL, (uint64_t)0x000000008000000aULL,
    (uint64_t)0x000000008000808bULL, (uint64_t)0x800000000000008bULL,
    (uint64_t)0x8000000000008089ULL, (uint64_t)0x8000000000008003ULL,
    (uint64_t)0x8000000000008002ULL, (uint64_t)0x8000000000000080ULL,
    (uint64_t)0x000000000000800aULL, (uint64_t)0x800000008000000aULL,
    (uint64_t)0x8000000080008081ULL, (uint64_t)0x8000000000008080ULL,
    (uint64_t)0x0000000080000001ULL, (uint64_t)0x8000000080008008ULL
};

static const int keccakf_rotc[24] = {
     1,  3,   6, 10, 15, 21, 28, 36, 45, 55,
    2, 14,  27, 41, 56,  8, 25, 43, 62, 18,
    39, 61, 20, 44
};

static const int keccakf_piln[24] = {
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21,
    24, 4, 15, 23, 19, 13, 12, 2, 20, 14,
    22, 9, 6, 1
};

void keccakf(uint64_t st[25]) {
    int i, j, round;
    uint64_t t, bc[5];

    for (round = 0; round < 24; round++) {
        for (i = 0; i < 5; i++)
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];

        for (i = 0; i < 5; i++) {
            t = bc[(i + 4) % 5] ^ ROL64(bc[(i + 1) % 5], 1);
            for (j = 0; j < 25; j += 5)
                st[j + i] ^= t;
        }

        t = st[1];
        for (i = 0; i < 24; i++) {
            j = keccakf_piln[i];
            bc[0] = st[j];
            st[j] = ROL64(t, keccakf_rotc[i]);
            t = bc[0];
        }

        for (j = 0; j < 25; j += 5) {
            for (i = 0; i < 5; i++)
                bc[i] = st[j + i];
            for (i = 0; i < 5; i++)
                st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }

        st[0] ^= keccakf_rndc[round];
    }
}

void keccak(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    uint64_t st[25] = {0};
    uint8_t temp[144] = {0};
    size_t i, rsiz = 200 - 2 * outlen, rsizw = rsiz / 8;
    const uint8_t *p = in;

    while (inlen >= rsiz) {
        for (i = 0; i < rsiz; i++)
            temp[i] = *p++;
        for (i = 0; i < rsizw; i++)
            st[i] ^= ((uint64_t *)temp)[i];
        keccakf(st);
        inlen -= rsiz;
    }

    // padding
    for (i = 0; i < inlen; i++)
        temp[i] = p[i];
    temp[i++] = 0x01;
    temp[rsiz - 1] |= 0x80;

    for (i = 0; i < rsizw; i++)
        st[i] ^= ((uint64_t *)temp)[i];

    keccakf(st);

    for (i = 0; i < outlen; i++)
        out[i] = ((uint8_t *)st)[i];
}
