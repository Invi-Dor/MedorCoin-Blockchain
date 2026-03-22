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


FILE: src/net/serialization.cpp

#include "net/serialization.h"
#include "crypto/verify_signature.h"
#include "transaction.h"
#include "block.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

// =============================================================================
// VERSION
// =============================================================================
static constexpr uint32_t CURRENT_TX_VERSION    = 1;
static constexpr uint32_t CURRENT_BLOCK_VERSION = 1;
static constexpr uint32_t MIN_TX_VERSION        = 1;
static constexpr uint32_t MIN_BLOCK_VERSION     = 1;

// =============================================================================
// LIMITS
// =============================================================================
static constexpr size_t   MAX_TX_DATA_BYTES     = 128 * 1024;
static constexpr size_t   MAX_TX_PER_BLOCK      = 10'000;
static constexpr size_t   MAX_ADDRESS_LEN       = 128;
static constexpr size_t   MAX_HASH_LEN          = 128;
static constexpr size_t   MAX_SIGNATURE_LEN     = 512;
static constexpr size_t   MAX_SIG_COMPONENT_B64 = 96;
static constexpr uint64_t MEDORCOIN_CHAIN_ID    = 0;

// =============================================================================
// STRUCTURED LOGGER
// =============================================================================
static std::mutex         g_log_mutex;
static SerializationLogFn g_log_fn;

void serializationSetLogger(SerializationLogFn fn) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    g_log_fn = std::move(fn);
}

static void slog(int level, const char* fn, const char* msg) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    if (g_log_fn) g_log_fn(level, fn, msg);
}

// =============================================================================
// METRICS
// =============================================================================
static std::atomic<uint64_t> g_tx_serialize_ok{0};
static std::atomic<uint64_t> g_tx_serialize_err{0};
static std::atomic<uint64_t> g_tx_deserialize_ok{0};
static std::atomic<uint64_t> g_tx_deserialize_err{0};
static std::atomic<uint64_t> g_block_serialize_ok{0};
static std::atomic<uint64_t> g_block_deserialize_ok{0};
static std::atomic<uint64_t> g_block_deserialize_err{0};
static std::atomic<uint64_t> g_sig_verify_fail{0};
static std::atomic<uint64_t> g_replay_rejected{0};

SerializationMetrics getSerializationMetrics() {
    return {
        g_tx_serialize_ok.load(std::memory_order_relaxed),
        g_tx_serialize_err.load(std::memory_order_relaxed),
        g_tx_deserialize_ok.load(std::memory_order_relaxed),
        g_tx_deserialize_err.load(std::memory_order_relaxed),
        g_block_serialize_ok.load(std::memory_order_relaxed),
        g_block_deserialize_ok.load(std::memory_order_relaxed),
        g_block_deserialize_err.load(std::memory_order_relaxed),
        g_sig_verify_fail.load(std::memory_order_relaxed),
        g_replay_rejected.load(std::memory_order_relaxed)
    };
}

// =============================================================================
// ERROR HELPER
// =============================================================================
static void throwErr(SerializationErrorCode code,
                     const char* fn,
                     const std::string& msg)
{
    slog(0, fn, msg.c_str());
    throw SerializationError(code, msg);
}

// =============================================================================
// REPLAY CACHE
// =============================================================================
static constexpr uint64_t REPLAY_WINDOW_SECONDS = 3600;

struct ReplayCache {
    mutable std::shared_mutex               mu;
    std::unordered_map<std::string, uint64_t> seen;

    bool check(const std::string& txHash) {
        uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now()
                    .time_since_epoch()).count());

        std::unique_lock<std::shared_mutex> lk(mu);
        for (auto it = seen.begin(); it != seen.end(); ) {
            if (now - it->second > REPLAY_WINDOW_SECONDS)
                it = seen.erase(it);
            else
                ++it;
        }
        auto [iter, inserted] = seen.emplace(txHash, now);
        if (!inserted) {
            iter->second = now;
            return false;
        }
        return true;
    }
};

static ReplayCache g_replay_cache;

// =============================================================================
// BASE64
// Single correct encoder. No duplicate. No broken padding logic.
// Handles all input lengths including those not divisible by 3.
// =============================================================================
static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const int8_t B64_TABLE[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static std::string b64enc(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        uint32_t v = (static_cast<uint32_t>(data[i])     << 16) |
                     (static_cast<uint32_t>(data[i + 1]) <<  8) |
                      static_cast<uint32_t>(data[i + 2]);
        result += B64_CHARS[(v >> 18) & 0x3F];
        result += B64_CHARS[(v >> 12) & 0x3F];
        result += B64_CHARS[(v >>  6) & 0x3F];
        result += B64_CHARS[ v        & 0x3F];
        i += 3;
    }
    if (i < len) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len)
            v |= static_cast<uint32_t>(data[i + 1]) << 8;
        result += B64_CHARS[(v >> 18) & 0x3F];
        result += B64_CHARS[(v >> 12) & 0x3F];
        result += (i + 1 < len) ? B64_CHARS[(v >> 6) & 0x3F] : '=';
        result += '=';
    }
    return result;
}

static std::string b64encVec(const std::vector<uint8_t>& v) {
    return b64enc(v.data(), v.size());
}

static std::string b64encArr32(const std::array<uint8_t, 32>& a) {
    return b64enc(a.data(), 32);
}

static std::vector<uint8_t> b64dec(const std::string& s, const char* ctx) {
    std::vector<uint8_t> result;
    result.reserve((s.size() / 4) * 3 + 3);
    uint32_t buf     = 0;
    int      bits    = 0;
    bool     padSeen = false;

    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '=') { padSeen = true; continue; }
        if (padSeen)
            throwErr(SerializationErrorCode::BadBase64, ctx,
                     std::string(ctx) + ": data after Base64 padding");
        int8_t val = B64_TABLE[c];
        if (val < 0)
            throwErr(SerializationErrorCode::BadBase64, ctx,
                     std::string(ctx) + ": invalid Base64 character");
        buf = (buf << 6) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return result;
}

// =============================================================================
// TRANSACTION HASH
// =============================================================================
static void computeTxContentHash(const Transaction& tx,
                                  unsigned char      out32[32])
{
    struct Guard {
        EVP_MD_CTX* p;
        Guard()  { p = EVP_MD_CTX_new(); }
        ~Guard() { if (p) EVP_MD_CTX_free(p); }
    } g;

    if (!g.p)
        throwErr(SerializationErrorCode::InternalError,
                 "computeTxContentHash", "EVP_MD_CTX_new failed");

    auto feed = [&](const void* d, size_t n) {
        if (EVP_DigestUpdate(g.p, d, n) != 1)
            throwErr(SerializationErrorCode::InternalError,
                     "computeTxContentHash", "EVP_DigestUpdate failed");
    };

    if (EVP_DigestInit_ex(g.p, EVP_sha256(), nullptr) != 1)
        throwErr(SerializationErrorCode::InternalError,
                 "computeTxContentHash", "EVP_DigestInit_ex failed");

    uint32_t ver = CURRENT_TX_VERSION;
    feed(&ver,                     sizeof(ver));
    feed(&tx.chainId,              sizeof(tx.chainId));
    feed(&tx.nonce,                sizeof(tx.nonce));
    feed(tx.toAddress.data(),      tx.toAddress.size());
    feed(&tx.value,                sizeof(tx.value));
    feed(&tx.gasLimit,             sizeof(tx.gasLimit));
    feed(&tx.maxFeePerGas,         sizeof(tx.maxFeePerGas));
    feed(&tx.maxPriorityFeePerGas, sizeof(tx.maxPriorityFeePerGas));
    uint64_t dataLen = tx.data.size();
    feed(&dataLen,                 sizeof(dataLen));
    if (!tx.data.empty())
        feed(tx.data.data(),       tx.data.size());

    unsigned int outLen = 32;
    if (EVP_DigestFinal_ex(g.p, out32, &outLen) != 1)
        throwErr(SerializationErrorCode::InternalError,
                 "computeTxContentHash", "EVP_DigestFinal_ex failed");
}

// =============================================================================
// FIELD VALIDATORS
// =============================================================================
static void requireField(const json& j, const char* field, const char* ctx) {
    if (!j.contains(field))
        throwErr(SerializationErrorCode::MissingField, ctx,
                 std::string(ctx) + ": missing field '" + field + "'");
}

static std::string getString(const json& j, const char* field,
                              size_t maxLen, const char* ctx) {
    requireField(j, field, ctx);
    if (!j.at(field).is_string())
        throwErr(SerializationErrorCode::TypeMismatch, ctx,
                 std::string(ctx) + ": '" + field + "' must be string");
    std::string val = j.at(field).get<std::string>();
    if (val.size() > maxLen)
        throwErr(SerializationErrorCode::LengthExceeded, ctx,
                 std::string(ctx) + ": '" + field + "' exceeds "
                 + std::to_string(maxLen) + " chars");
    return val;
}

static uint64_t getUint64(const json& j, const char* field,
                           const char* ctx) {
    requireField(j, field, ctx);
    if (!j.at(field).is_number_unsigned())
        throwErr(SerializationErrorCode::TypeMismatch, ctx,
                 std::string(ctx) + ": '" + field
                 + "' must be unsigned integer");
    return j.at(field).get<uint64_t>();
}

static uint32_t getUint32(const json& j, const char* field,
                           const char* ctx) {
    requireField(j, field, ctx);
    if (!j.at(field).is_number_unsigned())
        throwErr(SerializationErrorCode::TypeMismatch, ctx,
                 std::string(ctx) + ": '" + field
                 + "' must be unsigned integer");
    return j.at(field).get<uint32_t>();
}

static std::vector<uint8_t> getSigComp(const json& j, const char* field,
                                        const char* ctx) {
    requireField(j, field, ctx);
    if (!j.at(field).is_string())
        throwErr(SerializationErrorCode::TypeMismatch, ctx,
                 std::string(ctx) + ": '" + field + "' must be string");
    std::string enc = j.at(field).get<std::string>();
    if (enc.size() > MAX_SIG_COMPONENT_B64)
        throwErr(SerializationErrorCode::LengthExceeded, ctx,
                 std::string(ctx) + ": sig component '" + field
                 + "' exceeds limit");
    return b64dec(enc, ctx);
}

static bool isValidHexAddress(const std::string& addr) {
    size_t start = 0;
    if (addr.size() >= 2 && addr[0] == '0' &&
        (addr[1] == 'x' || addr[1] == 'X'))
        start = 2;
    if (addr.empty() || addr.size() - start != 40) return false;
    for (size_t i = start; i < addr.size(); i++) {
        char c = addr[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

// =============================================================================
// TRANSACTION SERIALIZATION
// =============================================================================
json serializeTx(const Transaction& tx) {
    try {
        json j;
        j["version"]              = CURRENT_TX_VERSION;
        j["txHash"]               = tx.txHash;
        j["toAddress"]            = tx.toAddress;
        j["value"]                = tx.value;
        j["gasLimit"]             = tx.gasLimit;
        j["maxFeePerGas"]         = tx.maxFeePerGas;
        j["maxPriorityFeePerGas"] = tx.maxPriorityFeePerGas;
        j["nonce"]                = tx.nonce;
        j["chainId"]              = tx.chainId;
        j["v"]                    = tx.v;
        j["r"]                    = b64encArr32(tx.r);
        j["s"]                    = b64encArr32(tx.s);
        j["data"]                 = b64encVec(tx.data);
        g_tx_serialize_ok.fetch_add(1, std::memory_order_relaxed);
        return j;
    } catch (...) {
        g_tx_serialize_err.fetch_add(1, std::memory_order_relaxed);
        throw;
    }
}

// =============================================================================
// TRANSACTION DESERIALIZATION
// =============================================================================
Transaction deserializeTx(const json& j) {
    static constexpr const char* CTX = "deserializeTx";
    Transaction tx;
    try {
        uint32_t ver = getUint32(j, "version", CTX);
        if (ver < MIN_TX_VERSION || ver > CURRENT_TX_VERSION)
            throwErr(SerializationErrorCode::VersionMismatch, CTX,
                     std::string(CTX) + ": unsupported tx version "
                     + std::to_string(ver));

        tx.txHash               = getString(j, "txHash",    MAX_HASH_LEN,    CTX);
        tx.toAddress            = getString(j, "toAddress", MAX_ADDRESS_LEN, CTX);
        tx.value                = getUint64(j, "value",                      CTX);
        tx.gasLimit             = getUint64(j, "gasLimit",                   CTX);
        tx.maxFeePerGas         = getUint64(j, "maxFeePerGas",               CTX);
        tx.maxPriorityFeePerGas = getUint64(j, "maxPriorityFeePerGas",       CTX);
        tx.nonce                = getUint64(j, "nonce",                      CTX);
        tx.chainId              = getUint64(j, "chainId",                    CTX);
        tx.v                    = getUint64(j, "v",                          CTX);

        auto rv = getSigComp(j, "r", CTX);
        auto sv = getSigComp(j, "s", CTX);
        if (rv.size() != 32 || sv.size() != 32)
            throwErr(SerializationErrorCode::SignatureInvalid, CTX,
                     "r and s must each be 32 bytes");
        std::copy(rv.begin(), rv.end(), tx.r.begin());
        std::copy(sv.begin(), sv.end(), tx.s.begin());

        requireField(j, "data", CTX);
        if (!j.at("data").is_string())
            throwErr(SerializationErrorCode::TypeMismatch, CTX,
                     "field 'data' must be string");
        tx.data = b64dec(j.at("data").get<std::string>(), CTX);

        if (!isValidHexAddress(tx.toAddress))
            throwErr(SerializationErrorCode::InvalidAddress, CTX,
                     "toAddress is not a valid 20-byte hex address: "
                     + tx.toAddress);

        if (tx.data.size() > MAX_TX_DATA_BYTES)
            throwErr(SerializationErrorCode::LengthExceeded, CTX,
                     "tx.data exceeds 128 KB limit");

        if (tx.chainId != MEDORCOIN_CHAIN_ID) {
            g_replay_rejected.fetch_add(1, std::memory_order_relaxed);
            throwErr(SerializationErrorCode::ChainIdMismatch, CTX,
                     "chainId " + std::to_string(tx.chainId)
                     + " != network chainId "
                     + std::to_string(MEDORCOIN_CHAIN_ID));
        }

        if (!g_replay_cache.check(tx.txHash)) {
            g_replay_rejected.fetch_add(1, std::memory_order_relaxed);
            throwErr(SerializationErrorCode::ReplayDetected, CTX,
                     "replay detected: txHash " + tx.txHash);
        }

        unsigned char computedHash[32];
        computeTxContentHash(tx, computedHash);

        char hexHash[65];
        for (int i = 0; i < 32; i++)
            snprintf(hexHash + i * 2, 3, "%02x", computedHash[i]);
        hexHash[64] = '\0';

        if (tx.txHash != std::string(hexHash))
            throwErr(SerializationErrorCode::HashMismatch, CTX,
                     "txHash does not match computed hash");

        int recoveryId = crypto::computeRecoveryId(tx.v, tx.chainId);
        if (recoveryId < 0)
            throwErr(SerializationErrorCode::SignatureInvalid, CTX,
                     "v field " + std::to_string(tx.v)
                     + " invalid for chainId "
                     + std::to_string(tx.chainId));

        unsigned char sig64[64];
        memcpy(sig64,      tx.r.data(), 32);
        memcpy(sig64 + 32, tx.s.data(), 32);

        unsigned char recoveredPubkey[33];
        if (!crypto::recoverPubkey(
                std::span<const unsigned char, 32>(computedHash, 32),
                std::span<const unsigned char, 64>(sig64, 64),
                recoveryId,
                std::span<unsigned char, 33>(recoveredPubkey, 33))) {
            g_sig_verify_fail.fetch_add(1, std::memory_order_relaxed);
            throwErr(SerializationErrorCode::SignatureInvalid, CTX,
                     "failed to recover public key");
        }

        if (!crypto::verifyHashWithPubkey(
                std::span<const unsigned char, 32>(computedHash, 32),
                std::span<const unsigned char, 33>(recoveredPubkey, 33),
                std::span<const unsigned char, 64>(sig64, 64))) {
            g_sig_verify_fail.fetch_add(1, std::memory_order_relaxed);
            throwErr(SerializationErrorCode::SignatureInvalid, CTX,
                     "ECDSA signature verification failed");
        }

        g_tx_deserialize_ok.fetch_add(1, std::memory_order_relaxed);
        return tx;

    } catch (const SerializationError&) {
        g_tx_deserialize_err.fetch_add(1, std::memory_order_relaxed);
        throw;
    } catch (const json::exception& e) {
        g_tx_deserialize_err.fetch_add(1, std::memory_order_relaxed);
        throwErr(SerializationErrorCode::InternalError, CTX,
                 std::string("JSON error: ") + e.what());
    } catch (const std::bad_alloc&) {
        g_tx_deserialize_err.fetch_add(1, std::memory_order_relaxed);
        throwErr(SerializationErrorCode::OutOfMemory, CTX,
                 "out of memory during transaction deserialization");
    }
    return tx;
}

// =============================================================================
// BLOCK SERIALIZATION
// =============================================================================
json serializeBlock(const Block& block) {
    json j;
    j["version"]      = CURRENT_BLOCK_VERSION;
    j["previousHash"] = block.previousHash;
    j["timestamp"]    = block.timestamp;
    j["hash"]         = block.hash;
    j["signature"]    = block.signature;
    j["baseFee"]      = block.baseFee;
    j["gasUsed"]      = block.gasUsed;
    j["reward"]       = block.reward;

    json txArr = json::array();
    txArr.get_ref<json::array_t&>().reserve(block.transactions.size());
    for (const auto& tx : block.transactions)
        txArr.push_back(serializeTx(tx));
    j["transactions"] = std::move(txArr);

    g_block_serialize_ok.fetch_add(1, std::memory_order_relaxed);
    return j;
}

// =============================================================================
// BLOCK DESERIALIZATION
// =============================================================================
Block deserializeBlock(const json& j) {
    static constexpr const char* CTX = "deserializeBlock";
    Block block;
    try {
        uint32_t ver = getUint32(j, "version", CTX);
        if (ver < MIN_BLOCK_VERSION || ver > CURRENT_BLOCK_VERSION)
            throwErr(SerializationErrorCode::VersionMismatch, CTX,
                     std::string(CTX) + ": unsupported block version "
                     + std::to_string(ver));

        block.previousHash = getString(j, "previousHash", MAX_HASH_LEN,      CTX);
        block.hash         = getString(j, "hash",         MAX_HASH_LEN,      CTX);
        block.signature    = getString(j, "signature",    MAX_SIGNATURE_LEN, CTX);
        block.timestamp    = getUint64(j, "timestamp", CTX);
        block.baseFee      = getUint64(j, "baseFee",   CTX);
        block.gasUsed      = getUint64(j, "gasUsed",   CTX);
        block.reward       = getUint64(j, "reward",    CTX);

        requireField(j, "transactions", CTX);
        if (!j.at("transactions").is_array())
            throwErr(SerializationErrorCode::TypeMismatch, CTX,
                     "field 'transactions' must be an array");

        const auto& txArr = j.at("transactions");
        if (txArr.size() > MAX_TX_PER_BLOCK)
            throwErr(SerializationErrorCode::BlockTooLarge, CTX,
                     "block has " + std::to_string(txArr.size())
                     + " transactions, max is "
                     + std::to_string(MAX_TX_PER_BLOCK));

        block.transactions.reserve(txArr.size());

        std::unordered_set<std::string> seenHashes;
        seenHashes.reserve(txArr.size());
        std::unordered_map<std::string, uint64_t> lastNonce;
        lastNonce.reserve(txArr.size());

        for (const auto& txJson : txArr) {
            Transaction tx = deserializeTx(txJson);

            if (!seenHashes.insert(tx.txHash).second)
                throwErr(SerializationErrorCode::DuplicateTx, CTX,
                         "duplicate txHash: " + tx.txHash);

            auto it = lastNonce.find(tx.toAddress);
            if (it != lastNonce.end()) {
                if (tx.nonce <= it->second)
                    throwErr(SerializationErrorCode::NonceOrdering, CTX,
                             "out-of-order nonce for " + tx.toAddress
                             + " got " + std::to_string(tx.nonce)
                             + " expected > " + std::to_string(it->second));
                it->second = tx.nonce;
            } else {
                lastNonce.emplace(tx.toAddress, tx.nonce);
            }

            block.transactions.push_back(std::move(tx));
        }

        g_block_deserialize_ok.fetch_add(1, std::memory_order_relaxed);
        return block;

    } catch (const SerializationError&) {
        g_block_deserialize_err.fetch_add(1, std::memory_order_relaxed);
        throw;
    } catch (const json::exception& e) {
        g_block_deserialize_err.fetch_add(1, std::memory_order_relaxed);
        throwErr(SerializationErrorCode::InternalError, CTX,
                 std::string("JSON error: ") + e.what());
    } catch (const std::bad_alloc&) {
        g_block_deserialize_err.fetch_add(1, std::memory_order_relaxed);
        throwErr(SerializationErrorCode::OutOfMemory, CTX,
                 "out of memory during block deserialization");
    }
    return block;
}


Paste both files into GitHub, commit, then send Group 2.​​​​​​​​​​​​​​​​
