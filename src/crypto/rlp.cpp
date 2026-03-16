#include "crypto/rlp.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace rlp {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

// Bounds-checked byte access — throws std::runtime_error on overrun so
// every decode path is protected without repetitive manual checks.
static inline uint8_t safeByte(const uint8_t *data,
                                 size_t         length,
                                 size_t         index,
                                 const char    *context)
{
    if (index >= length)
        throw std::runtime_error(
            std::string("[RLP] ") + context
            + ": unexpected end of input at offset "
            + std::to_string(index)
            + " (total length " + std::to_string(length) + ")");
    return data[index];
}

// Appends a length-prefix header into out.
// offset = 0x80 for byte strings, 0xC0 for lists.
static void encodeLength(size_t                len,
                          uint8_t               offset,
                          std::vector<uint8_t> &out)
{
    if (len < 56) {
        out.push_back(static_cast<uint8_t>(offset + len));
        return;
    }

    // Encode the length in the minimum number of big-endian bytes
    uint8_t  lenBuf[8];
    unsigned lenBytes = 0;
    for (size_t t = len; t > 0; t >>= 8)
        lenBuf[lenBytes++] = static_cast<uint8_t>(t & 0xFF);

    if (lenBytes > 8)
        throw std::runtime_error(
            "[RLP] encodeLength: payload exceeds maximum encodable size");

    // lenBuf is little-endian; reverse to big-endian
    std::reverse(lenBuf, lenBuf + lenBytes);

    out.push_back(static_cast<uint8_t>(offset + 55 + lenBytes));
    out.insert(out.end(), lenBuf, lenBuf + lenBytes);
}

// Decodes the length and header size at position offset in data[0..length).
// Returns the offset of the first payload byte.
static size_t decodeLength(const uint8_t *data,
                             size_t         length,
                             size_t         offset,
                             uint8_t        baseOffset,  // 0x80 or 0xC0
                             size_t        &payloadLen,
                             size_t        &headerLen,
                             const char    *context)
{
    const uint8_t prefix = safeByte(data, length, offset, context);
    const uint8_t shortLimit = baseOffset + 55;

    if (prefix >= baseOffset && prefix <= shortLimit) {
        payloadLen = prefix - baseOffset;
        headerLen  = 1;
        return offset + 1;
    }

    if (prefix > shortLimit) {
        const size_t lol = static_cast<size_t>(prefix - shortLimit);
        if (offset + 1 + lol > length)
            throw std::runtime_error(
                std::string("[RLP] ") + context
                + ": truncated length-of-length at offset "
                + std::to_string(offset));

        size_t decodedLen = 0;
        for (size_t i = 0; i < lol; ++i)
            decodedLen = (decodedLen << 8) | data[offset + 1 + i];

        if (decodedLen < 56)
            throw std::runtime_error(
                std::string("[RLP] ") + context
                + ": non-canonical long encoding at offset "
                + std::to_string(offset));

        payloadLen = decodedLen;
        headerLen  = 1 + lol;
        return offset + 1 + lol;
    }

    throw std::runtime_error(
        std::string("[RLP] ") + context
        + ": prefix 0x" + std::to_string(prefix)
        + " is not valid for base offset 0x"
        + std::to_string(baseOffset)
        + " at position " + std::to_string(offset));
}

// ─────────────────────────────────────────────────────────────────────────────
// Encoding — all functions append into a caller-supplied output buffer
// ─────────────────────────────────────────────────────────────────────────────

void encodeBytes(const uint8_t        *data,
                  size_t                len,
                  std::vector<uint8_t> &out)
{
    if (len == 1 && data[0] < 0x80) {
        // Single byte in [0x00, 0x7f] — encoded as itself, no length prefix
        out.push_back(data[0]);
        return;
    }
    encodeLength(len, 0x80, out);
    out.insert(out.end(), data, data + len);
}

void encodeBytes(const std::vector<uint8_t> &input,
                  std::vector<uint8_t>       &out)
{
    encodeBytes(input.empty() ? nullptr : input.data(), input.size(), out);
}

void encodeBytes32(const std::array<uint8_t, 32> &input,
                    std::vector<uint8_t>           &out)
{
    encodeBytes(input.data(), 32, out);
}

void encodeUInt(uint64_t value, std::vector<uint8_t> &out)
{
    if (value == 0) {
        // Zero encodes as empty byte string
        out.push_back(0x80);
        return;
    }

    // Minimal big-endian encoding — no leading zeros
    uint8_t  buf[8];
    unsigned n = 0;
    for (uint64_t v = value; v > 0; v >>= 8)
        buf[n++] = static_cast<uint8_t>(v & 0xFF);
    std::reverse(buf, buf + n);
    encodeBytes(buf, n, out);
}

void encodeUInt256(const uint256_t &value, std::vector<uint8_t> &out)
{
    // Find the first non-zero byte to produce minimal encoding
    size_t start = 0;
    while (start < 32 && value[start] == 0) ++start;

    if (start == 32) {
        // Value is zero
        out.push_back(0x80);
        return;
    }

    encodeBytes(value.data() + start, 32 - start, out);
}

void encodeList(const std::vector<std::vector<uint8_t>> &encodedItems,
                 std::vector<uint8_t>                    &out)
{
    // Compute total payload size up front to write a single correct header
    size_t payloadSize = 0;
    for (const auto &item : encodedItems)
        payloadSize += item.size();

    encodeLength(payloadSize, 0xC0, out);
    for (const auto &item : encodedItems)
        out.insert(out.end(), item.begin(), item.end());
}

void encodeValue(const RlpValue &value, std::vector<uint8_t> &out)
{
    if (value.isBytes()) {
        encodeBytes(value.asBytes(), out);
        return;
    }

    // List — recursively encode each child, then wrap with list header
    std::vector<uint8_t> payload;
    for (const auto &child : value.asList())
        encodeValue(child, payload);

    encodeLength(payload.size(), 0xC0, out);
    out.insert(out.end(), payload.begin(), payload.end());
}

// ─────────────────────────────────────────────────────────────────────────────
// Decoding — all functions operate on raw pointer + length
// ─────────────────────────────────────────────────────────────────────────────

RlpBytes decodeBytes(const uint8_t *data,
                      size_t         length,
                      size_t         offset,
                      size_t        &nextOffset)
{
    if (offset >= length)
        throw std::runtime_error(
            "[RLP] decodeBytes: offset " + std::to_string(offset)
            + " is out of range (length=" + std::to_string(length) + ")");

    const uint8_t prefix = data[offset];

    if (prefix < 0x80) {
        // Single byte in [0x00, 0x7f]
        nextOffset = offset + 1;
        return {prefix};
    }

    if (prefix > 0xBF)
        throw std::runtime_error(
            "[RLP] decodeBytes: expected byte string but found list prefix 0x"
            + std::to_string(prefix)
            + " at offset " + std::to_string(offset));

    size_t payloadLen = 0, headerLen = 0;
    const size_t dataStart = decodeLength(
        data, length, offset, 0x80, payloadLen, headerLen, "decodeBytes");

    if (dataStart + payloadLen > length)
        throw std::runtime_error(
            "[RLP] decodeBytes: payload extends beyond input at offset "
            + std::to_string(offset)
            + " (need " + std::to_string(payloadLen)
            + " bytes, have " + std::to_string(length - dataStart) + ")");

    nextOffset = dataStart + payloadLen;
    return RlpBytes(data + dataStart, data + dataStart + payloadLen);
}

uint64_t decodeUInt64(const uint8_t *data,
                       size_t         length,
                       size_t         offset,
                       size_t        &nextOffset)
{
    const RlpBytes bytes = decodeBytes(data, length, offset, nextOffset);

    if (bytes.size() > 8)
        throw std::runtime_error(
            "[RLP] decodeUInt64: encoded integer is "
            + std::to_string(bytes.size())
            + " bytes, maximum supported is 8");

    if (bytes.size() > 1 && bytes[0] == 0x00)
        throw std::runtime_error(
            "[RLP] decodeUInt64: non-canonical encoding with leading zero");

    uint64_t value = 0;
    for (auto b : bytes)
        value = (value << 8) | b;
    return value;
}

uint256_t decodeUInt256(const uint8_t *data,
                         size_t         length,
                         size_t         offset,
                         size_t        &nextOffset)
{
    const RlpBytes bytes = decodeBytes(data, length, offset, nextOffset);

    if (bytes.size() > 32)
        throw std::runtime_error(
            "[RLP] decodeUInt256: encoded integer is "
            + std::to_string(bytes.size())
            + " bytes, maximum supported is 32");

    if (bytes.size() > 1 && bytes[0] == 0x00)
        throw std::runtime_error(
            "[RLP] decodeUInt256: non-canonical encoding with leading zero");

    uint256_t value{};
    // Right-align into 32 bytes (big-endian, zero-padded on the left)
    const size_t pad = 32 - bytes.size();
    std::memcpy(value.data() + pad, bytes.data(), bytes.size());
    return value;
}

// Forward declaration for mutual recursion between decodeList and decodeValue
RlpValue decodeValue(const uint8_t *data, size_t length,
                      size_t offset, size_t &nextOffset);

RlpList decodeList(const uint8_t *data,
                    size_t         length,
                    size_t         offset,
                    size_t        &nextOffset)
{
    if (offset >= length)
        throw std::runtime_error(
            "[RLP] decodeList: offset " + std::to_string(offset)
            + " is out of range (length=" + std::to_string(length) + ")");

    const uint8_t prefix = data[offset];
    if (prefix < 0xC0)
        throw std::runtime_error(
            "[RLP] decodeList: expected list prefix >= 0xC0 but found 0x"
            + std::to_string(prefix)
            + " at offset " + std::to_string(offset));

    size_t payloadLen = 0, headerLen = 0;
    const size_t listStart = decodeLength(
        data, length, offset, 0xC0, payloadLen, headerLen, "decodeList");

    if (listStart + payloadLen > length)
        throw std::runtime_error(
            "[RLP] decodeList: list payload extends beyond input at offset "
            + std::to_string(offset));

    RlpList items;
    size_t  pos = listStart;
    const size_t end = listStart + payloadLen;

    while (pos < end) {
        size_t   childNext = 0;
        // Each element is decoded recursively — nested lists are handled
        // correctly because decodeValue inspects the prefix and dispatches
        // to either decodeBytes or decodeList as appropriate.
        RlpValue child = decodeValue(data, length, pos, childNext);

        if (childNext <= pos)
            throw std::runtime_error(
                "[RLP] decodeList: decoder made no progress at offset "
                + std::to_string(pos) + " — malformed input");

        items.push_back(std::move(child));
        pos = childNext;
    }

    if (pos != end)
        throw std::runtime_error(
            "[RLP] decodeList: list payload was not fully consumed (expected "
            + std::to_string(end) + ", stopped at " + std::to_string(pos) + ")");

    nextOffset = end;
    return items;
}

RlpValue decodeValue(const uint8_t *data,
                      size_t         length,
                      size_t         offset,
                      size_t        &nextOffset)
{
    if (offset >= length)
        throw std::runtime_error(
            "[RLP] decodeValue: offset " + std::to_string(offset)
            + " is out of range (length=" + std::to_string(length) + ")");

    const uint8_t prefix = data[offset];

    if (prefix >= 0xC0) {
        // List item — decode recursively
        RlpList list = decodeList(data, length, offset, nextOffset);
        return RlpValue::fromList(std::move(list));
    }

    // Byte string
    RlpBytes bytes = decodeBytes(data, length, offset, nextOffset);
    return RlpValue::fromBytes(std::move(bytes));
}

} // namespace rlp
