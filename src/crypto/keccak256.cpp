#include "keccak256.h"

namespace crypto {

static inline uint64_t rotl64(uint64_t x, unsigned int n) {
    return (x << n) | (x >> (64 - n));
}

static const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
    0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000000000001ULL, 0x8000000000008008ULL
};

static const unsigned RHO[25] = {
     0,  1, 62, 28, 27,
    36, 44,  6, 55, 20,
     3, 10, 43, 25, 39,
    41, 45, 15, 21,  8,
    18,  2, 61, 56, 14
};

static void keccakF(std::array<uint64_t,25>& st) {
    for (int r = 0; r < 24; r++) {
        uint64_t C[5], D[5], B[25];

        for (int x = 0; x < 5; x++)
            C[x] = st[x] ^ st[x+5] ^ st[x+10] ^ st[x+15] ^ st[x+20];

        for (int x = 0; x < 5; x++)
            D[x] = C[(x+4)%5] ^ rotl64(C[(x+1)%5], 1);

        for (int x = 0; x < 25; x++)
            st[x] ^= D[x%5];

        for (int i = 0; i < 25; i++)
            B[i] = rotl64(st[i], RHO[i]);

        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                st[x + 5*y] = B[y + 5*((2*x+3*y)%5)];

        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++)
                st[x + 5*y] ^= (~B[((x+1)%5) + 5*y]) & B[((x+2)%5) + 5*y];

        st[0] ^= RC[r];
    }
}

std::vector<uint8_t> Keccak256(const std::vector<uint8_t>& data) {
    constexpr size_t rate = 136; // 200-byte state - 64-byte capacity
    std::array<uint64_t,25> st{};
    std::array<uint8_t,200> block{};
    size_t i = 0;

    // Absorb full blocks
    while (i + rate <= data.size()) {
        for (size_t j = 0; j < rate/8; ++j) {
            uint64_t v;
            std::memcpy(&v, data.data() + i + j*8, 8);
            st[j] ^= v;
        }
        keccakF(st);
        i += rate;
    }

    // Pad remaining bytes
    std::memset(block.data(), 0, block.size());
    for (size_t j = i; j < data.size(); j++)
        block[j - i] = data[j];

    block[data.size() - i] ^= 0x01;   // padding start
    block[rate - 1] ^= 0x80;          // padding end

    for (size_t j = 0; j < rate/8; j++) {
        uint64_t v;
        std::memcpy(&v, block.data() + j*8, 8);
        st[j] ^= v;
    }

    keccakF(st);

    std::vector<uint8_t> out(32);
    for (size_t i = 0; i < 4; i++) { // 4*8 = 32 bytes
        uint64_t t = st[i];
        for (size_t b = 0; b < 8; b++)
            out[i*8 + b] = (t >> (8*b)) & 0xFF;
    }

    return out;
}

} // namespace crypto
