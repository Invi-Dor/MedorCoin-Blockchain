#ifndef EVM_SIGN_H
#define EVM_SIGN_H

#include <vector>
#include <array>
#include <cstdint>
#include "signature.h"

namespace crypto {
std::vector<uint8_t> signEvmTransaction(
    const std::vector<uint8_t>& rlpUnsignedTx,
    const std::array<uint8_t,32>& privKey
);
}

#endif // EVM_SIGN_H
