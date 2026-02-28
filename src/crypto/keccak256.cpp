#include "keccak256.h"
#include <array>
#include <cstring>
#include <algorithm>

namespace crypto {

class Keccak256Hasher {
public:
    static constexpr size_t HASH_SIZE = 32;
    Keccak256Hasher() noexcept : state_{}, rate_(136), pos_(0) {
        std::memset(state_.data(), 0, state_.size());
    }

    void update(const uint8_t* data, size_t length) noexcept {
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

    std::array<uint8_t, HASH_SIZE> digest() noexcept {
        state_[pos_] ^= 0x01;
        state_[rate_ - 1] ^= 0x80;
        keccakf1600(state_);
        std::array<uint8_t, HASH_SIZE> result;
        std::memcpy(result.data(), state_.data(), HASH_SIZE);
        return result;
    }

private:
    void keccakf1600(std::array<uint8_t, 200>& state) noexcept;
    std::array<uint8_t, 200> state_;
    size_t rate_;
    size_t pos_;
};

static inline uint64_t rotl64(uint64_t x, unsigned int n) {
    return (x << n) | (x >> (64 - n));
}

void Keccak256Hasher::keccakf1600(std::array<uint8_t, 200>& state) noexcept {
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

    const uint32_t RHO_OFFSETS[25] = {
         0,  1, 62, 28, 27,
        36, 44,  6, 55, 20,
         3, 10, 43, 25, 39,
        41, 45, 15, 21,  8,
        18,  2, 61, 56, 26
    };

    for (int round = 0; round < 24; ++round) {
        uint64_t C[5], D[5];
        for (int x = 0; x < 5; ++x)
            C[x] = A[x] ^ A[x+5] ^ A[x+10] ^ A[x+15] ^ A[x+20];
        for (int x = 0; x < 5; ++x)
            D[x] = C[(x+4)%5] ^ rotl64(C[(x+1)%5], 1);
        for (int x = 0; x < 5; ++x)
            for (int y = 0; y < 5; ++y)
                A[x + 5*y] ^= D[x];

        uint64_t B[25];
        for (int x = 0; x < 5; ++x)
            for (int y = 0; y < 5; ++y)
                B[y + ((2*x + 3*y) % 5) * 5] =
                rotl64(A[x + 5*y], RHO_OFFSETS[x + 5*y]);

        for (int x = 0; x < 5; ++x)
            for (int y = 0; y < 5; ++y)
                A[x + 5*y] = B[x + 5*y] ^
                             ((~B[((x+1)%5) + 5*y]) &
                               B[((x+2)%5) + 5*y]);
        A[0] ^= RC[round];
    }

    for (size_t i = 0; i < 25; ++i) {
        uint64_t v = A[i];
        for (int b = 0; b < 8; ++b)
            state[i*8 + b] = (uint8_t)((v >> (8*b)) & 0xFF);
    }
}

std::vector<unsigned char> Keccak256(const std::vector<unsigned char>& data) {
    Keccak256Hasher hasher;
    hasher.update(data.data(), data.size());
    auto arr = hasher.digest();
    return std::vector<unsigned char>(arr.begin(), arr.end());
}

} // namespace crypto
