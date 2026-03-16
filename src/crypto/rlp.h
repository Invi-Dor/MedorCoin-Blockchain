#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

/**
 * RLP (Recursive Length Prefix) encoder and decoder.
 *
 * Conforms to the Ethereum RLP specification:
 * https://ethereum.org/en/developers/docs/data-structures-and-encoding/rlp/
 *
 * Design guarantees:
 *  - Arbitrarily nested lists are supported through a recursive RlpValue
 *    type. decodeList no longer assumes all items are byte strings — each
 *    element is decoded to its correct type (bytes or nested list) based
 *    on its prefix byte.
 *  - 256-bit integers are supported via encodeUInt256 and decodeUInt256
 *    using a 32-byte big-endian array, compatible with Ethereum uint256.
 *  - Encoding functions write into a caller-supplied output buffer instead
 *    of always constructing a new vector, eliminating the bulk of temporary
 *    allocations in hot paths.
 *  - All decode functions propagate errors via std::runtime_error with
 *    descriptive messages rather than returning silent defaults.
 *  - The streaming decode interface accepts a raw pointer and length so
 *    callers are not forced to copy data into a vector before decoding.
 *  - No method depends on global state. All functions are thread-safe.
 */
namespace rlp {

// ── RlpValue — recursive variant type ────────────────────────────────────────
//
// An RlpValue is either a byte string or a list of RlpValues.
// This allows arbitrary nesting depth consistent with the RLP specification.

struct RlpValue;
using RlpBytes = std::vector<uint8_t>;
using RlpList  = std::vector<RlpValue>;

struct RlpValue {
    std::variant<RlpBytes, RlpList> data;

    bool isList()  const noexcept { return std::holds_alternative<RlpList>(data); }
    bool isBytes() const noexcept { return std::holds_alternative<RlpBytes>(data); }

    const RlpBytes &asBytes() const { return std::get<RlpBytes>(data); }
    const RlpList  &asList()  const { return std::get<RlpList>(data); }

    static RlpValue fromBytes(RlpBytes b) { RlpValue v; v.data = std::move(b); return v; }
    static RlpValue fromList (RlpList  l) { RlpValue v; v.data = std::move(l); return v; }
};

// ── 256-bit integer type (big-endian, 32 bytes) ───────────────────────────────
using uint256_t = std::array<uint8_t, 32>;

// ─────────────────────────────────────────────────────────────────────────────
// Encoding API
//
// All encode functions append into an existing output buffer supplied by
// the caller. This eliminates intermediate vector construction and allows
// callers to build up an entire RLP payload in a single pre-reserved buffer.
// ─────────────────────────────────────────────────────────────────────────────

// Encode raw bytes (RLP string)
void encodeBytes  (const uint8_t          *data, size_t len,
                   std::vector<uint8_t>   &out);
void encodeBytes  (const std::vector<uint8_t> &input,
                   std::vector<uint8_t>       &out);
void encodeBytes32(const std::array<uint8_t,32> &input,
                   std::vector<uint8_t>          &out);

// Encode unsigned integers — minimal big-endian encoding, no leading zeros
void encodeUInt  (uint64_t value, std::vector<uint8_t> &out);
void encodeUInt256(const uint256_t &value, std::vector<uint8_t> &out);

// Encode a list — items must already be individually encoded and passed in
// as a flat sequence of RLP-encoded byte spans
void encodeList(const std::vector<std::vector<uint8_t>> &encodedItems,
                std::vector<uint8_t>                    &out);

// Encode a full RlpValue tree (recursive)
void encodeValue(const RlpValue &value, std::vector<uint8_t> &out);

// ─────────────────────────────────────────────────────────────────────────────
// Decoding API
//
// All decode functions operate on a raw pointer + length so callers are
// not forced to copy data into a std::vector. The offset parameter marks
// where to begin reading; nextOffset is set to the first byte after the
// decoded item on success.
//
// All functions throw std::runtime_error on malformed input.
// ─────────────────────────────────────────────────────────────────────────────

// Decode a single byte string
RlpBytes decodeBytes(const uint8_t *data, size_t length,
                     size_t offset, size_t &nextOffset);

// Decode a single uint64 — throws if the encoded integer exceeds 8 bytes
uint64_t decodeUInt64(const uint8_t *data, size_t length,
                      size_t offset, size_t &nextOffset);

// Decode a 256-bit integer — throws if the encoded integer exceeds 32 bytes
uint256_t decodeUInt256(const uint8_t *data, size_t length,
                         size_t offset, size_t &nextOffset);

// Decode a list — returns RlpList whose elements may themselves be lists
RlpList decodeList(const uint8_t *data, size_t length,
                   size_t offset, size_t &nextOffset);

// Decode any RLP item (bytes or list) into an RlpValue tree (recursive)
RlpValue decodeValue(const uint8_t *data, size_t length,
                     size_t offset, size_t &nextOffset);

// ── Vector-input convenience wrappers ─────────────────────────────────────────
inline RlpBytes decodeBytes(const std::vector<uint8_t> &data,
                             size_t offset, size_t &nextOffset)
{
    return decodeBytes(data.data(), data.size(), offset, nextOffset);
}

inline uint64_t decodeUInt64(const std::vector<uint8_t> &data,
                              size_t offset, size_t &nextOffset)
{
    return decodeUInt64(data.data(), data.size(), offset, nextOffset);
}

inline uint256_t decodeUInt256(const std::vector<uint8_t> &data,
                                size_t offset, size_t &nextOffset)
{
    return decodeUInt256(data.data(), data.size(), offset, nextOffset);
}

inline RlpList decodeList(const std::vector<uint8_t> &data,
                           size_t offset, size_t &nextOffset)
{
    return decodeList(data.data(), data.size(), offset, nextOffset);
}

inline RlpValue decodeValue(const std::vector<uint8_t> &data,
                             size_t offset, size_t &nextOffset)
{
    return decodeValue(data.data(), data.size(), offset, nextOffset);
}

} // namespace rlp
