#pragma once

#include "transaction.h"
#include "block.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

// =============================================================================
// SERIALIZATION VERSION
// Increment SERIALIZATION_VERSION when the wire format changes.
// deserializeBlock and deserializeTx reject records with versions
// outside the supported range.
// =============================================================================
static constexpr uint32_t SERIALIZATION_VERSION = 1;

// =============================================================================
// STRUCTURED LOGGER
// Install once at node startup. Receives (level, function, message).
// Level: 0=info, 1=warn, 2=error. Default discards all messages.
// Thread-safe.
// =============================================================================
using SerializationLogFn =
    std::function<void(int, const char*, const char*)>;
void serializationSetLogger(SerializationLogFn fn);

// =============================================================================
// METRICS
// All counters are monotonically increasing.
// Thread-safe -- backed by std::atomic.
// =============================================================================
struct SerializationMetrics {
    uint64_t txSerializeOk;
    uint64_t txSerializeErr;
    uint64_t txDeserializeOk;
    uint64_t txDeserializeErr;
    uint64_t blockSerializeOk;
    uint64_t blockDeserializeOk;
    uint64_t blockDeserializeErr;
    uint64_t sigVerifyFail;
    uint64_t replayRejected;
};
SerializationMetrics getSerializationMetrics();

// =============================================================================
// ERROR CODES
// Structured error codes for programmatic handling.
// SerializationError inherits std::runtime_error so callers
// can catch either type.
// =============================================================================
enum class SerializationErrorCode : uint8_t {
    None             = 0,
    MissingField     = 1,
    TypeMismatch     = 2,
    LengthExceeded   = 3,
    InvalidAddress   = 4,
    ChainIdMismatch  = 5,
    VersionMismatch  = 6,
    HashMismatch     = 7,
    SignatureInvalid = 8,
    ReplayDetected   = 9,
    BadBase64        = 10,
    OutOfMemory      = 11,
    InternalError    = 12,
    DuplicateTx      = 13,
    NonceOrdering    = 14,
    BlockTooLarge    = 15
};

struct SerializationError : public std::runtime_error {
    SerializationErrorCode code;
    SerializationError(SerializationErrorCode c,
                        const std::string&     msg)
        : std::runtime_error(msg), code(c) {}
};

// =============================================================================
// SERIALIZATION API
//
// serializeTx    -- Transaction  → json
// deserializeTx  -- json         → Transaction
//   Validates: version, chainId, address, data size,
//              inputs, outputs, hash integrity,
//              ECDSA signature, replay cache.
//
// serializeBlock   -- Block → json
// deserializeBlock -- json  → Block
//   Validates: version, all block fields, transaction array,
//              duplicate txHash, nonce ordering per address,
//              block size limit.
//
// All functions throw SerializationError on failure.
// All functions are thread-safe.
// =============================================================================
json        serializeTx     (const Transaction& tx);
Transaction deserializeTx   (const json&        j);
json        serializeBlock  (const Block&       block);
Block       deserializeBlock(const json&        j);
