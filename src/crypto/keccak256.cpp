#include "keccak256.h"
#include <array>
#include <cstring>

namespace crypto {

static inline uint64_t rotl64(uint64_t x, unsigned int y) {
    return (x << y) | (x >> (64 - y));
}

// Round constants for Keccak-f1600
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

// Rho offsets
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
            C[x] = state[x] ^ state[x + 5] ^ state[x + 10] ^ state[x + 15] ^ state[x + 20];
        for (int x = 0; x < 5; x++)
            D[x] = C[(x + 4) % 5] ^ rotl64(C[(x + 1) % 5], 1);

        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                state[x + 5*y] ^= D[x];

        for (int i = 0; i < 25; i++)
            B[i] = rotl64(state[(i % 5) + 5*(i / 5)], KECCAKF_RHO_OFFSETS[i]);

        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                state[x + 5*y] = B[y + 5*((2*x + 3*y) % 5)];

        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++)
                state[x + 5*y] ^= (~B[((x + 1) % 5) + 5*y]) & B[((x + 2) % 5) + 5*y];

        state[0] ^= KECCAKF_ROUND_CONSTANTS[round];
    }
}

std::vector<unsigned char> Keccak256(const std::vector<unsigned char>& data) {
    constexpr size_t RATE_BYTES = 136;
    std::array<uint8_t, 200> byteState{};
    std::array<uint64_t, 25> state64{};
    std::memset(state64.data(), 0, state64.size() * sizeof(uint64_t));

    size_t offset = 0;
    while (offset + RATE_BYTES <= data.size()) {
        for (size_t i = 0; i < RATE_BYTES / 8; i++)
            state64[i] ^= ((uint64_t*)data.data())[i];
        keccakF1600(state64);
        offset += RATE_BYTES;
    }

    for (size_t i = offset; i < data.size(); i++)
        byteState[i % RATE_BYTES] ^= data[i];
    byteState[data.size() % RATE_BYTES] ^= 0x01;
    byteState[RATE_BYTES - 1] ^= 0x80;

    for (size_t i = 0; i < RATE_BYTES / 8; i++)
        state64[i] ^= ((uint64_t*)byteState.data())[i];
    keccakF1600(state64);

    std::vector<unsigned char> hash(32);
    std::memcpy(hash.data(), state64.data(), 32);
    return hash;
}

} // namespace crypto
