#ifndef SIGNATURE_H
#define SIGNATURE_H

#include <array>
#include <tuple>
#include <cstdint>

namespace crypto {

std::tuple<
    std::array<uint8_t,32>,
    std::array<uint8_t,32>,
    uint8_t
> signHash(
    const std::array<uint8_t,32>& digest,
    const std::array<uint8_t,32>& privKeyBytes
);

bool verifyHash(
    const std::array<uint8_t,32>& digest,
    const std::array<uint8_t,32>& sig64,
    uint8_t recid,
    const std::array<uint8_t,33>& pubkeyBytes
);

bool recoverPubkey(
    const std::array<uint8_t,32>& digest,
    const std::array<uint8_t,32>& sig64,
    uint8_t recid,
    std::array<uint8_t,33>& outPubkey
);

} // namespace crypto

#endif // SIGNATURE_H
