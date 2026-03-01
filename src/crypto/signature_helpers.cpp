#include "signature_helpers.h"

namespace crypto {

uint64_t computeEip155V(int recid, uint64_t chainId) {
    // EIP‑155: v = recid + (chainId * 2 + 35)
    return static_cast<uint64_t>(recid) + (chainId * 2 + 35);
}

} // namespace crypto
