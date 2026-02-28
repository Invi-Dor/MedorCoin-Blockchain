// File: crypto/signature_helpers.h
// SPDX‑License‑Identifier: MIT
#ifndef CRYPTO_SIGNATURE_HELPERS_H
#define CRYPTO_SIGNATURE_HELPERS_H

#include <cstdint>

namespace crypto {

// Compute Ethereum EIP‑155 and EIP‑1559 compatible “v”:
uint64_t computeEip155V(int recid, uint64_t chainId);

} // namespace crypto

#endif // CRYPTO_SIGNATURE_HELPERS_H
