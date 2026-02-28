

// SPDX-License-Identifier: MIT

#include <iostream>
#include <vector>
#include <cstring>
#include <array>
#include <cstdint>
#include <algorithm>

namespace crypto {

class Keccak256 {
public:
    static constexpr size_t HASH_SIZE = 32; // 256 bits
    Keccak256() noexcept;
    void update(const uint8_t* data, size_t length) noexcept;
    std::array<uint8_t, HASH_SIZE> digest() noexcept;
    void reset() noexcept;

private:
    void keccakf1600(std::array<uint8_t, 200>& state) noexcept;

    std::array<uint8_t, 200> state_; // internal state (200 bytes for Keccak256)
    size_t rate_; // Keccak rate (136 bytes for Keccak256)
    size_t pos_;  // current position in the buffer
};

Keccak256::Keccak256() noexcept
    : state_(), rate_(136), pos_(0) {
    reset();
}

void Keccak256::reset() noexcept {
    std::memset(state_.data(), 0, state_.size());
    pos_ = 0;
}

void Keccak256::update(const uint8_t* data, size_t length) noexcept {
    size_t i = 0;
    while (length > 0) {
        size_t block_size = std::min(rate_ - pos_, length);
        std::memcpy(state_.data() + pos_, data + i, block_size);
        pos_ += block_size;
        i += block_size;
        length -= block_size;

        if (pos_ == rate_) {
            keccakf1600(state_);
            pos_ = 0;
        }
    }
}

std::array<uint8_t, Keccak256::HASH_SIZE> Keccak256::digest() noexcept {
    std::array<uint8_t, HASH_SIZE> result = {};
    // Padding with 0x01 byte at current pos and 0x80 at end (pad10*1)
    state_[pos_] ^= 0x01;
    state_[rate_ - 1] ^= 0x80;
    keccakf1600(state_);
    std::memcpy(result.data(), state_.data(), HASH_SIZE);
    return result;
}

// 64-bit left rotate
static inline uint64_t rotl64(uint64_t x, unsigned int n) {
    return (n < 64) ? ((x << n) | (x >> (64 - n))) : x;
}

// Keccak-f1600 permutation
void Keccak256::keccakf1600(std::array<uint8_t, 200>& state) noexcept {
    // Load state into 25 lanes (A[x + 5*y], 0 <= x,y < 5)
    uint64_t A[25];
    for (size_t i = 0; i < 25; ++i) {
        size_t idx = i * 8;
        A[i] = static_cast<uint64_t>(state[idx]) |
               (static_cast<uint64_t>(state[idx + 1]) << 8) |
               (static_cast<uint64_t>(state[idx + 2]) << 16) |
               (static_cast<uint64_t>(state[idx + 3]) << 24) |
               (static_cast<uint64_t>(state[idx + 4]) << 32) |
               (static_cast<uint64_t>(state[idx + 5]) << 40) |
               (static_cast<uint64_t>(state[idx + 6]) << 48) |
               (static_cast<uint64_t>(state[idx + 7]) << 56);
    }

    // Round constants for Keccak-f1600
    const uint64_t RC[24] = {
        0x0000000000000001ULL,
        0x0000000000008082ULL,
        0x800000000000808AULL,
        0x8000000080008000ULL,
        0x000000000000808BULL,
        0x0000000080000001ULL,
        0x8000000080008081ULL,
        0x8000000000008009ULL,
        0x000000000000008AULL,
        0x0000000000000088ULL,
        0x0000000080008009ULL,
        0x000000008000000AULL,
        0x000000008000808BULL,
        0x800000000000008BULL,
        0x8000000000008089ULL,
        0x8000000000008003ULL,
        0x8000000000008002ULL,
        0x8000000000000080ULL,
        0x000000000000800AULL,
        0x800000008000000AULL,
        0x8000000080008081ULL,
        0x8000000000008080ULL,
        0x0000000000000001ULL,
        0x8000000000008008ULL
    };

    // Rho offsets for each lane (x + 5*y)
    const uint32_t RHO_OFFSETS[25] = {
        0,   1,  62,  28,  27,
        36,  44,  6,  55,  20,
        3,   10, 43,  25,  39,
        41,  45, 15,  21,   8,
        18,   2, 61,  56,  26
    };

    for (int round = 0; round < 24; ++round) {
        // θ step
        uint64_t C[5];
        for (int x = 0; x < 5; ++x) {
            C[x] = A[x + 0*5] ^ A[x + 1*5] ^ A[x + 2*5] ^ A[x + 3*5] ^ A[x + 4*5];
        }
        uint64_t D[5];
        for (int x = 0; x < 5; ++x) {
            D[x] = C[(x + 4) % 5] ^ rotl64(C[(x + 1) % 5], 1);
        }
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                A[x + y*5] ^= D[x];
            }
        }

        // ρ and π
        uint64_t B[25];
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                size_t idxA = x + y*5;
                size_t idxB = y + ((2*x + 3*y) % 5) * 5;
                uint64_t val = A[idxA];
                int shift = static_cast<int>(RHO_OFFSETS[idxA]);
                B[idxB] = rotl64(val, shift);
            }
        }

        // χ
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                size_t idx = x + y*5;
                A[idx] = B[idx] ^ ((~B[((x + 1) % 5) + y*5]) & B[((x + 2) % 5) + y*5]);
            }
        }

        // ι
        A[0] ^= RC[round];
    }

    // Store back to state (little-endian)
    for (size_t i = 0; i < 25; ++i) {
        size_t idx = i * 8;
        state[idx]     = static_cast<uint8_t>(A[i] & 0xff);
        state[idx + 1] = static_cast<uint8_t>((A[i] >> 8) & 0xff);
        state[idx + 2] = static_cast<uint8_t>((A[i] >> 16) & 0xff);
        state[idx + 3] = static_cast<uint8_t>((A[i] >> 24) & 0xff);
        state[idx + 4] = static_cast<uint8_t>((A[i] >> 32) & 0xff);
        state[idx + 5] = static_cast<uint8_t>((A[i] >> 40) & 0xff);
        state[idx + 6] = static_cast<uint8_t>((A[i] >> 48) & 0xff);
        state[idx + 7] = static_cast<uint8_t>((A[i] >> 56) & 0xff);
    }
}

} // namespace crypto

