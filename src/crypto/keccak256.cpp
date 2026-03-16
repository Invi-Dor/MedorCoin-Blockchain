#include "crypto/keccak256.h"

#include <cstring>
#include <iostream>

namespace crypto {

// ─────────────────────────────────────────────────────────────────────────────
// Keccak-f[1600] constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t   KECCAK_RATE         = 136;   // bytes (1088-bit rate)
static constexpr uint8_t  KECCAK_DOMAIN_SEP   = 0x01;  // Ethereum Keccak, not NIST SHA-3
static constexpr size_t   STATE_LANES         = 25;

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

// Rho rotation offsets indexed by lane [x + 5*y]
static const unsigned RHO[25] = {
     0,  1, 62, 28, 27,
    36, 44,  6, 55, 20,
     3, 10, 43, 25, 39,
    41, 45, 15, 21,  8,
    18,  2, 61, 56, 14
};

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers — all inlined, no heap usage
// ─────────────────────────────────────────────────────────────────────────────

static inline uint64_t rotl64(uint64_t x, unsigned int n) noexcept
{
    return (x << n) | (x >> (64u - n));
}

// Portable little-endian read — correct on both little and big endian hosts
static inline uint64_t leRead64(const uint8_t *p) noexcept
{
    return static_cast<uint64_t>(p[0])
         | (static_cast<uint64_t>(p[1]) <<  8)
         | (static_cast<uint64_t>(p[2]) << 16)
         | (static_cast<uint64_t>(p[3]) << 24)
         | (static_cast<uint64_t>(p[4]) << 32)
         | (static_cast<uint64_t>(p[5]) << 40)
         | (static_cast<uint64_t>(p[6]) << 48)
         | (static_cast<uint64_t>(p[7]) << 56);
}

// Portable little-endian write
static inline void leWrite64(uint8_t *p, uint64_t v) noexcept
{
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// ─────────────────────────────────────────────────────────────────────────────
// Keccak-f[1600] permutation
// ─────────────────────────────────────────────────────────────────────────────

static void keccakF1600(uint64_t st[STATE_LANES]) noexcept
{
    for (int round = 0; round < 24; ++round) {

        // ── Theta ─────────────────────────────────────────────────────────────
        uint64_t C[5], D[5];
        for (int x = 0; x < 5; ++x)
            C[x] = st[x] ^ st[x+5] ^ st[x+10] ^ st[x+15] ^ st[x+20];
        for (int x = 0; x < 5; ++x)
            D[x] = C[(x+4)%5] ^ rotl64(C[(x+1)%5], 1);
        for (int i = 0; i < 25; ++i)
            st[i] ^= D[i % 5];

        // ── Rho + Pi ──────────────────────────────────────────────────────────
        uint64_t B[25];
        for (int i = 0; i < 25; ++i) {
            const int x = i % 5;
            const int y = i / 5;
            B[y + 5 * ((2*x + 3*y) % 5)] = rotl64(st[i], RHO[i]);
        }

        // ── Chi ───────────────────────────────────────────────────────────────
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x)
                st[x + 5*y] = B[x + 5*y]
                            ^ (~B[((x+1)%5) + 5*y] & B[((x+2)%5) + 5*y]);

        // ── Iota ──────────────────────────────────────────────────────────────
        st[0] ^= RC[round];
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Core implementation — operates on raw pointer, no heap allocation
// ─────────────────────────────────────────────────────────────────────────────

static void keccak256Core(const uint8_t   *data,
                           size_t           length,
                           Keccak256Digest &out) noexcept
{
    uint64_t state[STATE_LANES] = {};

    // ── Absorb full rate-sized blocks ──────────────────────────────────────
    while (length >= KECCAK_RATE) {
        for (size_t i = 0; i < KECCAK_RATE / 8; ++i)
            state[i] ^= leRead64(data + i * 8);
        keccakF1600(state);
        data   += KECCAK_RATE;
        length -= KECCAK_RATE;
    }

    // ── Pad and absorb final block ─────────────────────────────────────────
    // Stack-allocated — no heap involvement
    uint8_t pad[KECCAK_RATE] = {};
    std::memcpy(pad, data, length);
    pad[length]           ^= KECCAK_DOMAIN_SEP;  // 0x01 = Ethereum Keccak
    pad[KECCAK_RATE - 1]  ^= 0x80;               // End-of-padding marker

    for (size_t i = 0; i < KECCAK_RATE / 8; ++i)
        state[i] ^= leRead64(pad + i * 8);
    keccakF1600(state);

    // ── Squeeze 256 bits (32 bytes) ────────────────────────────────────────
    for (size_t i = 0; i < 4; ++i)
        leWrite64(out.data() + i * 8, state[i]);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

// Primary overload — zero allocation, caller supplies the output buffer.
// Returns false and fills out with zeroes if data is null with length > 0.
bool Keccak256(const uint8_t   *data,
               size_t           length,
               Keccak256Digest &digestOut) noexcept
{
    if (!data && length > 0) {
        std::cerr << "[Keccak256] null data pointer with length="
                  << length << " — returning zero digest\n";
        digestOut.fill(0);
        return false;
    }

    // data == nullptr with length == 0 is valid (hash of empty input)
    const uint8_t empty = 0;
    keccak256Core(data ? data : &empty, length, digestOut);
    return true;
}

// Vector-to-fixed-digest overload — zero allocation on hot paths
bool Keccak256(const std::vector<uint8_t> &data,
               Keccak256Digest            &digestOut) noexcept
{
    return Keccak256(data.empty() ? nullptr : data.data(),
                     data.size(),
                     digestOut);
}

// Convenience overload — returns heap vector; suitable for non-critical paths
std::vector<uint8_t> Keccak256(const uint8_t *data,
                                size_t         length) noexcept
{
    Keccak256Digest digest{};
    if (!Keccak256(data, length, digest))
        return std::vector<uint8_t>(32, 0);
    return std::vector<uint8_t>(digest.begin(), digest.end());
}

// Convenience overload — vector input to vector output
std::vector<uint8_t> Keccak256(const std::vector<uint8_t> &data) noexcept
{
    return Keccak256(data.empty() ? nullptr : data.data(), data.size());
}

} // namespace crypto
