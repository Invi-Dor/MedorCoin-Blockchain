ifndef CRYPTO_KECCAK256_H
define CRYPTO_KECCAK256_H

include <vector>
include <cstdint>

namespace crypto {
std::vector<uint8_t> Keccak256(const std::vector<uint8_t>& data);
} // namespace crypto

endif // KECCAK256_H
