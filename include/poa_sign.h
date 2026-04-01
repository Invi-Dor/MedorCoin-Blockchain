#pragma once

#include "block.h"
#include "transaction.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// =============================================================================
// CODEC NAMESPACE
// Wire protocol encoding and decoding.
// All functions are noexcept and allocation-safe.
// decodeFrame validates payload length before any allocation.
// =============================================================================
namespace codec {

enum class MessageType : uint8_t {
    Handshake        = 0x01,
    HandshakeAck     = 0x02,
    Ping             = 0x03,
    Pong             = 0x04,
    Block            = 0x10,
    Transaction      = 0x11,
    TransactionBatch = 0x12,
    GetBlocks        = 0x20,
    Inventory        = 0x21,
    GetData          = 0x22,
    NotFound         = 0x23,
    Headers          = 0x24,
    GetHeaders       = 0x25,
    PeerExchange     = 0x30,
    Version          = 0x40,
    VersionAck       = 0x41,
    Reject           = 0x50,
    Unknown          = 0xFF
};

static constexpr size_t   HEADER_BYTES     = 9;
static constexpr uint32_t MAX_FRAME_BYTES  = 32 * 1024 * 1024;
static constexpr uint32_t PROTOCOL_VERSION = 1;

struct Frame {
    MessageType          type    = MessageType::Unknown;
    std::vector<uint8_t> payload;
    uint32_t             version = PROTOCOL_VERSION;
};

enum class DecodeError : uint8_t {
    None           = 0,
    TooShort       = 1,
    BadMagic       = 2,
    OversizedFrame = 3,
    PayloadTrunc   = 4,
    AllocFailed    = 5,
    Unknown        = 255
};

struct DecodeResult {
    bool                 ok    = false;
    std::optional<Frame> frame;
    DecodeError          error = DecodeError::None;
};

bool         encodeFrame      (const Frame& f,
                                std::vector<uint8_t>& out)       noexcept;
DecodeResult decodeFrame      (const uint8_t* data,
