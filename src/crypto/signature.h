#ifndef SIGNATURE_H
#define SIGNATURE_H

#include <array>
#include <tuple>
#include <cstdint>

// Produces (r, s, v) from a 32‑byte Keccak hash and 32‑byte private key
std::tuple<
    std::array<uint8_t,32>,
    std::array<uint8_t,32>,
    uint8_t
> signHash(
    const std::array<uint8_t,32> &digest,
    const std::array<uint8_t,32> &privKeyBytes
);

#endif // SIGNATURE_H
