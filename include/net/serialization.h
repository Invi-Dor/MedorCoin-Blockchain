#pragma once

#include "transaction.h"
#include "block.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

static constexpr uint32_t SERIALIZATION_VERSION = 1;

using SerializationLogFn =
    std::function<void(int, const char*, const char*)>;
void serializationSetLogger(SerializationLogFn fn);

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
    SerializationError(SerializationErrorCode c, const std::string& msg)
        : std::runtime_error(msg), code(c) {}
};

json        serializeTx(const Transaction& tx);
Transaction deserializeTx(const json& j);
json        serializeBlock(const Block& block);
Block       deserializeBlock(const json& j);
