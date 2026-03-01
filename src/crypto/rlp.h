#ifndef RLP_H
#define RLP_H

#include <vector>
#include <cstdint>

/**
 * Recursive Length Prefix (RLP) encode functions.
 * This matches the Ethereum RLP specification:
 * strings and lists encoded according to length and payload rules.  [oai_citation:0‡ethereum.org](https://ethereum.org/developers/docs/data-structures-and-encoding/rlp/?utm_source=chatgpt.com)
 */
namespace rlp {

std::vector<uint8_t> encodeBytes(const std::vector<uint8_t>& input);
std::vector<uint8_t> encodeUInt(uint64_t value);
std::vector<uint8_t> encodeList(const std::vector<std::vector<uint8_t>>& items);

} // namespace rlp

#endif // RLP_H
