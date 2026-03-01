#include "keccak256.h"
#include <array>
#include <cstring>

namespace crypto {

static inline uint64_t rotl64(uint64_t x, unsigned int y) {
    return (x << y) | (x >> (64 - y));
}

static const uint64_t KECCAKF_ROUND_CONSTANTS[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
    0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000000000001ULL, 0x8000000000008008ULL
};

static const unsigned KECCAKF_RHO_OFFSETS[25] = {
     0,  1, 62, 28, 27,
    36, 44,  6, 55, 20,
     3, 10, 43, 25, 39,
    41, 45, 15, 21,  8,
    18,  2, 61, 56, 14
};

static void keccakF1600(std::array<uint64_t, 25>& state) {
    for (int round = 0; round < 24; ++round) {
        uint64_t C[5], D[5], B[25];

        for (int x = 0; x < 5; x++)
            C[x] = state[x + 0*5] ^ state[x + 1*5] ^ state[x + 2*5] ^ state[x + 3*5] ^ state[x + 4*5];
        for (int x = 0; x < 5; x++)
            D[x] = C[(x+4)%5] ^ rotl64(C[(x+1)%5], 1);

        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                state[x + 5*y] ^= D[x];

        for (int i = 0; i < 25; i++)
            B[i] = rotl64(state[i], KECCAKF_RHO_OFFSETS[i]);

        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                state[x + 5*y] = B[y + 5*((2*x + 3*y) % 5)];

        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++)
                state[x + 5*y] ^= (~B[((x+1)%5) + 5*y]) & B[((x+2)%5) + 5*y];

        state[0] ^= KECCAKF_ROUND_CONSTANTS[round];
    }
}

std::vector<unsigned char> Keccak256(const std::vector<unsigned char>& data) {
    constexpr size_t RATE_BYTES = 136;
    std::array<uint64_t, 25> state64 = {};
    std::array<uint8_t, 200> buf = {};
    size_t i = 0;

    while (i + RATE_BYTES <= data.size()) {
        for (size_t j = 0; j < RATE_BYTES / 8; j++)
            state64[j] ^= ((uint64_t*)data.data())[j];
        keccakF1600(state64);
        i += RATE_BYTES;
    }

    for (size_t j = i; j < data.size(); j++)
        buf[j % RATE_BYTES] ^= data[j];
    buf[data.size() % RATE_BYTES] ^= 0x01;
    buf[RATE_BYTES - 1] ^= 0x80;

    for (size_t j = 0; j < RATE_BYTES/8; j++)
        state64[j] ^= ((uint64_t*)buf.data())[j];
    keccakF1600(state64);

    std::vector<unsigned char> hash(32);
    std::memcpy(hash.data(), state64.data(), 32);

    return hash;
}

} // namespace crypto
