// kevcak.cpp
// Hardened Keccak (Ethereum Keccak-256 compatible core) with C API.
// Drop-in replacement for your prior Keccak.cpp, but safer and portable.
// Exposes: void keccakf(uint64_t st[25]); and int keccak(const uint8_t* in, size_t inlen, uint8_t* out, size_t outlen);
// If you previously used different names, adjust the function names below accordingly.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER)
  #include <intrin.h>
  static inline uint64_t rotl64(uint64_t x, int r) { return _rotl64(x, r); }
#else
  static inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
#endif

static inline uint64_t load_le64(const uint8_t* p) {
    return ((uint64_t)p[0])       |
           ((uint64_t)p[1] <<  8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static inline void store_le64(uint8_t* p, uint64_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}

static inline void secure_bzero(void* p, size_t n) {
#if defined(__STDC_LIB_EXT1__)
    memset_s(p, n, 0, n);
#else
    volatile uint8_t* vp = (volatile uint8_t*)p;
    while (n--) *vp++ = 0;
#endif
}

static const uint64_t keccakf_rndc[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL
};

static const int keccakf_rotc[24] = {
     1,  3,  6, 10, 15, 21, 28, 36, 45, 55,
     2, 14, 27, 41, 56,  8, 25, 43, 62, 18,
    39, 61, 20, 44
};

static const int keccakf_piln[24] = {
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21,
    24, 4, 15, 23, 19, 13, 12, 2, 20, 14,
    22, 9, 6, 1
};

void keccakf(uint64_t st[25]) {
    uint64_t bc[5];
    for (int round = 0; round < 24; ++round) {
        // Theta
        for (int i = 0; i < 5; ++i)
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        for (int i = 0; i < 5; ++i) {
            uint64_t t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5)
                st[j + i] ^= t;
        }

        // Rho + Pi
        uint64_t t = st[1];
        for (int i = 0; i < 24; ++i) {
            int j = keccakf_piln[i];
            uint64_t tmp = st[j];
            st[j] = rotl64(t, keccakf_rotc[i]);
            t = tmp;
        }

        // Chi
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; ++i) bc[i] = st[j + i];
            for (int i = 0; i < 5; ++i)
                st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }

        // Iota
        st[0] ^= keccakf_rndc[round];
    }
    secure_bzero(bc, sizeof(bc));
}

// Returns 1 on success, 0 on invalid args.
// outlen in bytes. Use 32 for Ethereum Keccak-256.
int keccak(const uint8_t* in, size_t inlen, uint8_t* out, size_t outlen) {
    if (!out || outlen == 0) return 0;
    if (!in && inlen != 0) return 0;
    if (outlen > 64) return 0; // conservative cap

    const size_t rate = 200U - 2U * outlen; // capacity = 2*outlen
    if (rate == 0 || (rate % 8U) != 0U) return 0;
    const size_t rate_words = rate / 8U;

    uint64_t st[25] = {0};
    uint8_t block[200];
    memset(block, 0, sizeof(block));

    const uint8_t* p = in;
    size_t rem = inlen;

    // Absorb full blocks
    while (rem >= rate) {
        for (size_t i = 0; i < rate_words; ++i)
            st[i] ^= load_le64(p + 8U * i);
        keccakf(st);
        p += rate;
        rem -= rate;
    }

    // Final block + Keccak padding (0x01 ... 0x80)
    if (rem) memcpy(block, p, rem);
    block[rem] = 0x01;
    block[rate - 1] |= 0x80;

    for (size_t i = 0; i < rate_words; ++i)
        st[i] ^= load_le64(block + 8U * i);
    keccakf(st);

    // Squeeze
    size_t produced = 0;
    while (produced < outlen) {
        size_t take = (outlen - produced < rate) ? (outlen - produced) : rate;

        size_t qwords = take / 8U;
        for (size_t i = 0; i < qwords; ++i)
            store_le64(out + produced + 8U * i, st[i]);

        if ((take & 7U) != 0U) {
            uint8_t tmp[8];
            store_le64(tmp, st[qwords]);
            memcpy(out + produced + 8U * qwords, tmp, take & 7U);
            secure_bzero(tmp, sizeof(tmp));
        }

        produced += take;
        if (produced < outlen) {
            keccakf(st);
        }
    }

    secure_bzero(st, sizeof(st));
    secure_bzero(block, rate);
    return 1;
}
