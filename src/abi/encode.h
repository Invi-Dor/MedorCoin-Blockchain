ifndef CRYPTO_RLP_H
define CRYPTO_RLP_H

include <vector>
include <cstdint>

namespace rlp {
std::vector<uint8_t> encodeBytes(const std::vector<uint8_t>& input);
std::vector<uint8_t> encodeUInt(uint64_t value);
std::vector<uint8_t> encodeList(const std::vector<std::vector<uint8_t>>& items);
} // namespace rlp

endif
