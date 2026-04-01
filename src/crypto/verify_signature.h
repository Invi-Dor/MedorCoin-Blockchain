#pragma once

#include <cstdint>
#include <functional>
#include <span>

namespace crypto {

using LogCallback = std::function<void(int, const char*, const char*)>;
void setVerifySignatureLogger(LogCallback cb);

bool verifyHashWithPubkey(
    std::span<const unsigned char, 32> hash32,
    std::span<const unsigned char, 33> pubkey33,
    std::span<const unsigned char, 64> sig64);

bool recoverPubkey(
    std::span<const unsigned char, 32> hash32,
    std::span<const unsigned char, 64> sig64,
    int                                recoveryId,
    std::span<unsigned char, 33>       pubkeyOut33);

bool recoverAndVerify(
    const uint8_t* hash32,
    const uint8_t* rBytes,
    const uint8_t* sBytes,
    int            v,
    const uint8_t* expectedAddr20);

int computeRecoveryId(uint64_t v, uint64_t chainId);

} // namespace crypto
