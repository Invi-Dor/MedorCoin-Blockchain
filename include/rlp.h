#pragma once
#include <vector>
#include <cstdint>
#include <string>

namespace rlp {

/**
 * Encode a single byte string (raw bytes).
 */
std::vector<uint8_t> encodeBytes(const std::vector<uint8_t>& input);

/**
 * Encode an unsigned integer in RLP (big‑endian no leading zeros).
 */
std::vector<uint8_t> encodeUInt(uint64_t value);

/**
 * Encode a list of RLP items.
 */
std::vector<uint8_t> encodeList(const std::vector<std::vector<uint8_t>>& items);

} // namespace rlp
