#include "rlp.h"
#include <algorithm>

namespace rlp {

// Internal helper: add length prefix
static void encodeLength(size_t len, uint8_t offset, std::vector<uint8_t>& out) {
    if (len < 56) {
        out.push_back(uint8_t(offset + len));
    } else {
        std::vector<uint8_t> tmp;
        while (len > 0) {
            tmp.push_back(uint8_t(len & 0xff));
            len >>= 8;
        }
        std::reverse(tmp.begin(), tmp.end());
        out.push_back(uint8_t(offset + 55 + tmp.size()));
        out.insert(out.end(), tmp.begin(), tmp.end());
    }
}

// Encodes a byte string
std::vector<uint8_t> encodeBytes(const std::vector<uint8_t>& input) {
    if (input.size() == 1 && input[0] < 0x80) {
        return input;
    }
    std::vector<uint8_t> out;
    encodeLength(input.size(), 0x80, out);
    out.insert(out.end(), input.begin(), input.end());
    return out;
}

// Encodes an unsigned integer
std::vector<uint8_t> encodeUInt(uint64_t value) {
    if (value == 0) {
        return {0x80};
    }
    std::vector<uint8_t> buf;
    while (value > 0) {
        buf.push_back(uint8_t(value & 0xff));
        value >>= 8;
    }
    std::reverse(buf.begin(), buf.end());
    return encodeBytes(buf);
}

// Encodes a list of RLP items
std::vector<uint8_t> encodeList(const std::vector<std::vector<uint8_t>>& items) {
    std::vector<uint8_t> payload;
    for (auto const &item : items) {
        payload.insert(payload.end(), item.begin(), item.end());
    }
    std::vector<uint8_t> out;
    encodeLength(payload.size(), 0xc0, out);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

} // namespace rlp
