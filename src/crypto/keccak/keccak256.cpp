// File: crypto/keccak256.cpp
// SPDX‑License‑Identifier: MIT
// Implementation: Keccak‑256 (Ethereum compatible).

#include "keccak256.h"
#include <array>

// Small, proven Keccak‑256 implementation.
// Based on public domain reference Keccak code (slightly adapted).

namespace crypto {

static const std::array<uint64_t, 24> KECCAKF_RNDC = {
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

static const std::array<int, 24> KECCAKF_ROTC = {
     1,  3,   6, 10, 15, 21, 28, 36, 45, 55,
     2, 14,  27, 41, 56,  8, 25, 43, 62, 18,
    39, 61,  20, 44
};

static const std::array<int, 24> KECCAKF_PILN = {
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21,
    24, 4, 15, 23, 19, 13, 12, 2, 20, 14,
    22, 9, 6, 1
};

static void keccakf(uint64_t st[25]) {
    for (int round = 0; round < 24; ++round) {
        // θ
        uint64_t bc[5];
        for (int i = 0; i < 5; ++i)
            bc[i] = st[i] ^ st[i+5] ^ st[i+10] ^ st[i+15] ^ st[i+20];
        for (int i = 0; i < 5; ++i) {
            uint64_t t = bc[(i+4)%5] ^ ((bc[(i+1)%5] << 1) | (bc[(i+1)%5] >> (64-1)));
            for (int j = 0; j < 25; j += 5)
                st[j+i] ^= t;
        }
        // ρ and π
        uint64_t t = st[1];
        for (int i = 0; i < 24; ++i) {
            int j = KECCAKF_PILN[i];
            uint64_t tmp = st[j];
            st[j] = (t << KECCAKF_ROTC[i]) | (t >> (64-KECCAKF_ROTC[i]));
            t = tmp;
        }
        // χ
        for (int j = 0; j < 25; j += 5) {
            uint64_t row[5];
            for (int i = 0; i < 5; ++i) row[i] = st[j+i];
            for (int i = 0; i < 5; ++i)
                st[j+i] ^= (~row[(i+1)%5]) & row[(i+2)%5];
        }
        // ι
        st[0] ^= KECCAKF_RNDC[round];
    }
}

std::vector<uint8_t> keccak256(const uint8_t* data, size_t len) {
    const size_t rate = 1088/8;
    const size_t capacity = 512/8;
    uint64_t st[25] = {0};
    std::vector<uint8_t> temp(rate);

    size_t offset = 0;
    while (len >= rate) {
        for (size_t i = 0; i < rate/8; ++i) {
            uint64_t v = 0;
            for (int b = 0; b < 8; ++b)
                v |= (uint64_t)data[offset + i*8 + b] << (8*b);
            st[i] ^= v;
        }
        keccakf(st);
        offset += rate;
        len -= rate;
    }

    std::vector<uint8_t> block(rate, 0);
    if (len) memcpy(block.data(), data+offset, len);
    block[len] = 0x01;
    block[rate-1] |= 0x80;

    for (size_t i = 0; i < rate/8; ++i) {
        uint64_t v = 0;
        for (int b = 0; b < 8; ++b)
            v |= (uint64_t)block[i*8 + b] << (8*b);
        st[i] ^= v;
    }
    keccakf(st);

    std::vector<uint8_t> hash(32);
    for (size_t i = 0; i < 4; ++i) {
        uint64_t v = st[i];
        for (int b = 0; b < 8; ++b)
            hash[i*8 + b] = (v >> (8*b)) & 0xFF;
    }
    return hash;
}

std::vector<uint8_t> keccak256(const std::vector<uint8_t>& input) {
    return keccak256(input.data(), input.size());
}

} // namespace crypto
