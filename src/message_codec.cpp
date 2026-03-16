#include "message_codec.h"

#include <cstring>
#include <iostream>

// ── Optional hardware CRC32C (x86 SSE4.2) ────────────────────────────────────
#if defined(__SSE4_2__) && (defined(__x86_64__) || defined(__i386__))
#  include <nmmintrin.h>
#  define MEDOR_HW_CRC32C 1
#endif

namespace codec {

// ─────────────────────────────────────────────────────────────────────────────
// Big-endian helpers — one canonical implementation used by every field
// ─────────────────────────────────────────────────────────────────────────────

static inline void beWrite32(uint8_t *p, uint32_t v) noexcept
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >>  8);
    p[3] = static_cast<uint8_t>(v);
}

static inline uint32_t beRead32(const uint8_t *p) noexcept
{
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) <<  8)
         |  static_cast<uint32_t>(p[3]);
}

static inline void beWrite64(uint8_t *p, uint64_t v) noexcept
{
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v);
        v >>= 8;
    }
}

static inline uint64_t beRead64(const uint8_t *p) noexcept
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// CRC32C — table-lookup (software) or SSE4.2 (hardware)
//
// The compile-time table is generated using the Castagnoli polynomial
// 0x82F63B78. Each byte requires exactly one table lookup and one XOR,
// giving roughly 500 MB/s on modern cores in software mode. The SSE4.2
// path processes 8 bytes per instruction for approximately 4 GB/s.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint32_t CRC32C_POLY = 0x82F63B78u;

static constexpr std::array<uint32_t, 256> buildCrcTable() noexcept
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (CRC32C_POLY & (0u - (crc & 1u)));
        table[i] = crc;
    }
    return table;
}

static constexpr auto CRC_TABLE = buildCrcTable();

uint32_t crc32c(const uint8_t *data, size_t len) noexcept
{
    if (!data && len > 0) return 0;

#if defined(MEDOR_HW_CRC32C)
    // Hardware path: process 8 bytes at a time using the _mm_crc32_u64 intrinsic.
    uint64_t crc64 = 0xFFFFFFFFu;
    const uint8_t *p = data;

    while (len >= 8) {
        uint64_t word = 0;
        std::memcpy(&word, p, 8);
        crc64 = _mm_crc32_u64(crc64, word);
        p   += 8;
        len -= 8;
    }
    uint32_t crc = static_cast<uint32_t>(crc64);
    while (len-- > 0)
        crc = (crc >> 8) ^ CRC_TABLE[(crc ^ *p++) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
#else
    // Software path: table-lookup, one byte per iteration
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ CRC_TABLE[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame encode
// ─────────────────────────────────────────────────────────────────────────────

bool encodeFrame(const Frame &frame, std::vector<uint8_t> &out) noexcept
{
    if (frame.payload.size() > MAX_FRAME_BYTES) {
        std::cerr << "[codec] encodeFrame: payload size "
                  << frame.payload.size()
                  << " exceeds MAX_FRAME_BYTES " << MAX_FRAME_BYTES << "\n";
        return false;
    }

    const uint32_t payLen   = static_cast<uint32_t>(frame.payload.size());
    const uint32_t checksum = crc32c(frame.payload.data(), frame.payload.size());

    out.reserve(out.size() + HEADER_BYTES + frame.payload.size());

    // 4 bytes: magic
    const size_t hdrStart = out.size();
    out.resize(out.size() + HEADER_BYTES);
    uint8_t *hdr = out.data() + hdrStart;

    beWrite32(hdr,     MAGIC);
    hdr[4] = static_cast<uint8_t>(frame.type);
    beWrite32(hdr + 5, payLen);
    beWrite32(hdr + 9, checksum);

    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return true;
}

std::optional<std::vector<uint8_t>> encodeFrameNew(const Frame &frame) noexcept
{
    std::vector<uint8_t> out;
    if (!encodeFrame(frame, out)) return std::nullopt;
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame decode — fully noexcept, returns typed DecodeResult
// ─────────────────────────────────────────────────────────────────────────────

DecodeResult decodeFrame(const uint8_t *buf, size_t len) noexcept
{
    if (!buf) return DecodeResult::failure(DecodeError::InternalError);
    if (len < HEADER_BYTES) return DecodeResult::incomplete();

    const uint32_t magic = beRead32(buf);
    if (magic != MAGIC) {
        std::cerr << "[codec] decodeFrame: bad magic 0x"
                  << std::hex << magic << std::dec << "\n";
        return DecodeResult::failure(DecodeError::BadMagic);
    }

    const uint8_t rawType = buf[4];

    // Validate type byte against all known values
    const MessageType type = static_cast<MessageType>(rawType);
    switch (type) {
        case MessageType::Transaction:
        case MessageType::Block:
        case MessageType::Ping:
        case MessageType::Pong:
        case MessageType::GetPeers:
        case MessageType::Peers:
        case MessageType::TransactionBatch:
            break;
        default:
            std::cerr << "[codec] decodeFrame: unknown type 0x"
                      << std::hex << static_cast<int>(rawType) << std::dec << "\n";
            return DecodeResult::failure(DecodeError::UnknownType);
    }

    const uint32_t payLen = beRead32(buf + 5);
    if (payLen > MAX_FRAME_BYTES) {
        std::cerr << "[codec] decodeFrame: payload length "
                  << payLen << " exceeds cap " << MAX_FRAME_BYTES << "\n";
        return DecodeResult::failure(DecodeError::FrameTooLarge);
    }

    if (len < HEADER_BYTES + payLen)
        return DecodeResult::incomplete();

    const uint32_t storedCRC  = beRead32(buf + 9);
    const uint32_t computedCRC = crc32c(buf + HEADER_BYTES, payLen);

    if (computedCRC != storedCRC) {
        std::cerr << "[codec] decodeFrame: CRC mismatch "
                     "(stored=0x" << std::hex << storedCRC
                  << " computed=0x" << computedCRC << std::dec << ")\n";
        return DecodeResult::failure(DecodeError::CrcMismatch);
    }

    Frame f;
    f.type = type;
    f.payload.assign(buf + HEADER_BYTES, buf + HEADER_BYTES + payLen);

    return DecodeResult::success(std::move(f), HEADER_BYTES + payLen);
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal encode helpers
// ─────────────────────────────────────────────────────────────────────────────

static void appendUint64(std::vector<uint8_t> &out, uint64_t v) noexcept
{
    const size_t pos = out.size();
    out.resize(pos + 8);
    beWrite64(out.data() + pos, v);
}

static void appendString(std::vector<uint8_t> &out, const std::string &s) noexcept
{
    const uint32_t len = static_cast<uint32_t>(s.size());
    const size_t   pos = out.size();
    out.resize(pos + 4 + s.size());
    beWrite32(out.data() + pos, len);
    if (!s.empty())
        std::memcpy(out.data() + pos + 4, s.data(), s.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal decode helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool readUint64Safe(const uint8_t *buf, size_t bufLen,
                            size_t &pos, uint64_t &out) noexcept
{
    if (pos + 8 > bufLen) return false;
    out = beRead64(buf + pos);
    pos += 8;
    return true;
}

static bool readStringSafe(const uint8_t *buf, size_t bufLen,
                            size_t &pos, std::string &out) noexcept
{
    if (pos + 4 > bufLen) return false;
    const uint32_t len = beRead32(buf + pos);
    pos += 4;
    // Guard against maliciously large string length field
    if (len > MAX_FRAME_BYTES) return false;
    if (pos + len > bufLen) return false;
    out.assign(reinterpret_cast<const char *>(buf + pos), len);
    pos += len;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Transaction encode / decode
// ─────────────────────────────────────────────────────────────────────────────

void encodeTransaction(const Transaction &tx,
                        std::vector<uint8_t> &out) noexcept
{
    // Pre-reserve: 7×8 (fixed fields) + 4+addr + 4+hash + 64 (r,s) + 4+data
    out.reserve(out.size()
                + 56
                + 4 + tx.toAddress.size()
                + 4 + tx.txHash.size()
                + 64
                + 4 + tx.data.size());

    appendUint64(out, tx.chainId);
    appendUint64(out, tx.nonce);
    appendUint64(out, tx.maxPriorityFeePerGas);
    appendUint64(out, tx.maxFeePerGas);
    appendUint64(out, tx.gasLimit);
    appendUint64(out, tx.value);
    appendUint64(out, tx.v);
    appendString(out, tx.toAddress);
    appendString(out, tx.txHash);

    // r and s — fixed 32 bytes each
    out.insert(out.end(), tx.r.begin(), tx.r.end());
    out.insert(out.end(), tx.s.begin(), tx.s.end());

    // data field with 4-byte length prefix
    const uint32_t dataLen = static_cast<uint32_t>(tx.data.size());
    const size_t   dpos    = out.size();
    out.resize(dpos + 4 + tx.data.size());
    beWrite32(out.data() + dpos, dataLen);
    if (!tx.data.empty())
        std::memcpy(out.data() + dpos + 4, tx.data.data(), tx.data.size());
}

std::optional<std::vector<uint8_t>> encodeTransactionNew(
    const Transaction &tx) noexcept
{
    std::vector<uint8_t> out;
    encodeTransaction(tx, out);
    return out;
}

std::optional<Transaction> decodeTransaction(const uint8_t *buf,
                                              size_t         len) noexcept
{
    if (!buf || len < 60) return std::nullopt;

    Transaction tx;
    size_t pos = 0;

    if (!readUint64Safe(buf, len, pos, tx.chainId))              return std::nullopt;
    if (!readUint64Safe(buf, len, pos, tx.nonce))                return std::nullopt;
    if (!readUint64Safe(buf, len, pos, tx.maxPriorityFeePerGas)) return std::nullopt;
    if (!readUint64Safe(buf, len, pos, tx.maxFeePerGas))         return std::nullopt;
    if (!readUint64Safe(buf, len, pos, tx.gasLimit))             return std::nullopt;
    if (!readUint64Safe(buf, len, pos, tx.value))                return std::nullopt;
    if (!readUint64Safe(buf, len, pos, tx.v))                    return std::nullopt;
    if (!readStringSafe(buf, len, pos, tx.toAddress))            return std::nullopt;
    if (!readStringSafe(buf, len, pos, tx.txHash))               return std::nullopt;

    // r and s — exactly 32 bytes each
    if (pos + 64 > len) return std::nullopt;
    std::memcpy(tx.r.data(), buf + pos, 32); pos += 32;
    std::memcpy(tx.s.data(), buf + pos, 32); pos += 32;

    // data field
    if (pos + 4 > len) return std::nullopt;
    const uint32_t dataLen = beRead32(buf + pos); pos += 4;
    if (dataLen > MAX_FRAME_BYTES) return std::nullopt;
    if (pos + dataLen > len) return std::nullopt;
    tx.data.assign(buf + pos, buf + pos + dataLen);

    return tx;
}

// ─────────────────────────────────────────────────────────────────────────────
// Block encode / decode
// ─────────────────────────────────────────────────────────────────────────────

void encodeBlock(const Block &block, std::vector<uint8_t> &out) noexcept
{
    out.reserve(out.size()
                + 4 + block.hash.size()
                + 4 + block.previousHash.size()
                + 24   // timestamp + reward + baseFee
                + 4    // txCount
                + block.transactions.size() * 300);

    appendString(out, block.hash);
    appendString(out, block.previousHash);
    appendUint64(out, block.timestamp);
    appendUint64(out, block.reward);
    appendUint64(out, block.baseFee);

    const uint32_t txCount = static_cast<uint32_t>(block.transactions.size());
    const size_t   cpos    = out.size();
    out.resize(cpos + 4);
    beWrite32(out.data() + cpos, txCount);

    for (const auto &tx : block.transactions) {
        // Reserve space for 4-byte length prefix + encoded tx
        const size_t beforeTx = out.size();
        out.resize(beforeTx + 4);      // placeholder for length
        encodeTransaction(tx, out);
        const uint32_t tlen = static_cast<uint32_t>(out.size() - beforeTx - 4);
        beWrite32(out.data() + beforeTx, tlen);
    }
}

std::optional<std::vector<uint8_t>> encodeBlockNew(const Block &block) noexcept
{
    std::vector<uint8_t> out;
    encodeBlock(block, out);
    return out;
}

std::optional<Block> decodeBlock(const uint8_t *buf, size_t len) noexcept
{
    if (!buf || len < 16) return std::nullopt;

    Block  block;
    size_t pos = 0;

    if (!readStringSafe(buf, len, pos, block.hash))         return std::nullopt;
    if (!readStringSafe(buf, len, pos, block.previousHash)) return std::nullopt;

    uint64_t ts = 0, rw = 0, bf = 0;
    if (!readUint64Safe(buf, len, pos, ts)) return std::nullopt; block.timestamp = ts;
    if (!readUint64Safe(buf, len, pos, rw)) return std::nullopt; block.reward    = rw;
    if (!readUint64Safe(buf, len, pos, bf)) return std::nullopt; block.baseFee   = bf;

    if (pos + 4 > len) return std::nullopt;
    const uint32_t txCount = beRead32(buf + pos); pos += 4;

    // Guard against a malicious txCount that would exhaust memory
    if (txCount > 100000) return std::nullopt;
    block.transactions.reserve(txCount);

    for (uint32_t i = 0; i < txCount; ++i) {
        if (pos + 4 > len) return std::nullopt;
        const uint32_t tlen = beRead32(buf + pos); pos += 4;
        if (tlen > MAX_FRAME_BYTES || pos + tlen > len) return std::nullopt;
        auto txOpt = decodeTransaction(buf + pos, tlen);
        if (!txOpt) return std::nullopt;
        block.transactions.push_back(std::move(*txOpt));
        pos += tlen;
    }

    return block;
}

} // namespace codec
