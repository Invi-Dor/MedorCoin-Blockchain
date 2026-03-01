#ifndef CRYPTO_EVM_SIGN_H
#define CRYPTO_EVM_SIGN_H

#include <vector>
#include <array>
#include <cstdint>

namespace crypto {

struct EvmTx {
    uint64_t chainId;
    uint64_t nonce;
    uint64_t maxPriorityFeePerGas;
    uint64_t maxFeePerGas;
    uint64_t gasLimit;
    std::vector<uint8_t> to;
    std::vector<uint8_t> value;
    std::vector<uint8_t> data;
    std::vector<uint8_t> r;
    std::vector<uint8_t> s;
    uint8_t v;
};

std::vector<uint8_t> signEvmTransaction(
    EvmTx &tx,
    const std::array<uint8_t,32> &privkey
);

} // namespace crypto

#endif // CRYPTO_EVM_SIGN_H
