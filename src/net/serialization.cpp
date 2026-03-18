#include "net/serialization.h"
#include "crypto/signature.h"
#include "transaction.h"
#include "block.h"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <unordered_set>
#include <algorithm>

using json = nlohmann::json;

// =============================================================================
// LIMITS
// =============================================================================
static constexpr size_t   MAX_TX_DATA_BYTES    = 128 * 1024;
static constexpr size_t   MAX_TX_PER_BLOCK     = 10000;
static constexpr size_t   MAX_ADDRESS_LEN      = 128;
static constexpr size_t   MAX_HASH_LEN         = 128;
static constexpr size_t   MAX_SIGNATURE_LEN    = 512;
static constexpr size_t   MAX_SIG_COMPONENT    = 96;
static constexpr uint64_t MEDORCOIN_CHAIN_ID   = 0;

// =============================================================================
// BASE64
// =============================================================================
static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t v = (static_cast<uint32_t>(data[i])     << 16) |
                     (static_cast<uint32_t>(data[i + 1]) <<  8) |
                      static_cast<uint32_t>(data[i + 2]);
        result += B64_CHARS[(v >> 18) & 0x3F];
        result += B64_CHARS[(v >> 12) & 0x3F];
        result += B64_CHARS[(v >>  6) & 0x3F];
        result += B64_CHARS[ v        & 0x3F];
        i += 3;
    }
    if (i < data.size()) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size())
            v |= static_cast<uint32_t>(data[i + 1]) << 8;
        result += B64_CHARS[(v >> 18) & 0x3F];
        result += B64_CHARS[(v >> 12) & 0x3F];
        result += (i + 1 < data.size()) ? B64_CHARS[(v >> 6) & 0x3F] : '=';
        result += '=';
    }
    return result;
}

static std::vector<uint8_t> base64Decode(const std::string& s) {
    static const int8_t TABLE[256] = {
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
    std::vector<uint8_t> result;
    result.reserve((s.size() / 4) * 3);
    uint32_t buf  = 0;
    int      bits = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '=') break;
        int8_t val = TABLE[static_cast<unsigned char>(c)];
        if (val < 0)
            throw std::runtime_error(
                "Invalid Base64 character in serialized field");
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
// SHA-256 of transaction contents for hash verification
// =============================================================================
static void computeTxHash(
    const Transaction& tx,
    unsigned char      out32[32])
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        throw std::runtime_error("EVP_MD_CTX_new failed in computeTxHash");

    auto feed = [&](const void* data, size_t len) {
        if (EVP_DigestUpdate(ctx, data, len) != 1)
            throw std::runtime_error("EVP_DigestUpdate failed in computeTxHash");
    };

    unsigned int outLen = 32;
    bool ok = (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1);
    if (ok) {
        feed(tx.toAddress.data(),  tx.toAddress.size());
        feed(&tx.value,            sizeof(tx.value));
        feed(&tx.gasLimit,         sizeof(tx.gasLimit));
        feed(&tx.maxFeePerGas,     sizeof(tx.maxFeePerGas));
        feed(&tx.maxPriorityFeePerGas, sizeof(tx.maxPriorityFeePerGas));
        feed(&tx.nonce,            sizeof(tx.nonce));
        feed(&tx.chainId,          sizeof(tx.chainId));
        if (!tx.data.empty())
            feed(tx.data.data(),   tx.data.size());
        ok = (EVP_DigestFinal_ex(ctx, out32, &outLen) == 1);
    }
    EVP_MD_CTX_free(ctx);
    if (!ok)
        throw std::runtime_error("SHA-256 failed in computeTxHash");
}

// =============================================================================
// FIELD VALIDATION HELPERS
// =============================================================================
static void requireString(const json& j, const char* field) {
    if (!j.contains(field))
        throw std::runtime_error(std::string("Missing field: ") + field);
    if (!j.at(field).is_string())
        throw std::runtime_error(
            std::string("Field must be a string: ") + field);
}

static void requireUnsigned(const json& j, const char* field) {
    if (!j.contains(field))
        throw std::runtime_error(std::string("Missing field: ") + field);
    if (!j.at(field).is_number_unsigned())
        throw std::runtime_error(
            std::string("Field must be an unsigned integer: ") + field);
}

static std::string getString(
    const json& j, const char* field, size_t maxLen)
{
    requireString(j, field);
    std::string val = j.at(field).get<std::string>();
    if (val.size() > maxLen)
        throw std::runtime_error(
            std::string("Field exceeds maximum length: ") + field);
    return val;
}

static uint64_t getUint64(const json& j, const char* field) {
    requireUnsigned(j, field);
    return j.at(field).get<uint64_t>();
}

static std::vector<uint8_t> getSigComponent(
    const json& j, const char* field)
{
    requireString(j, field);
    std::string encoded = j.at(field).get<std::string>();
    if (encoded.size() > MAX_SIG_COMPONENT)
        throw std::runtime_error(
            std::string("Signature component exceeds limit: ") + field);
    return base64Decode(encoded);
}

// =============================================================================
// ADDRESS FORMAT VALIDATION
// Validates that an address is a non-empty hex string of the correct length.
// Accepts both prefixed (0x...) and unprefixed hex.
// =============================================================================
static bool isValidHexAddress(const std::string& addr) {
    if (addr.empty()) return false;
    size_t start = 0;
    if (addr.size() >= 2 && addr[0] == '0' &&
        (addr[1] == 'x' || addr[1] == 'X'))
        start = 2;
    // Standard address: 40 hex chars (20 bytes)
    if (addr.size() - start != 40) return false;
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
    json j;
    j["txHash"]               = tx.txHash;
    j["toAddress"]            = tx.toAddress;
    j["value"]                = tx.value;
    j["gasLimit"]             = tx.gasLimit;
    j["maxFeePerGas"]         = tx.maxFeePerGas;
    j["maxPriorityFeePerGas"] = tx.maxPriorityFeePerGas;
    j["nonce"]                = tx.nonce;
    j["chainId"]              = tx.chainId;
    j["v"]                    = tx.v;
    j["r"]                    = base64Encode(tx.r);
    j["s"]                    = base64Encode(tx.s);
    j["data"]                 = base64Encode(tx.data);
    return j;
}

// =============================================================================
// TRANSACTION DESERIALIZATION
// Resolves all six production issues:
//   1. txHash verified against recomputed hash of contents
//   2. ECDSA signature verified via crypto::recoverPubkey
//   3. chainId validated against MEDORCOIN_CHAIN_ID
//   4. uint64_t fields validated as unsigned integers
//   5. toAddress validated as 40-char hex string
//   6. Uniqueness checked at block level (see deserializeBlock)
// =============================================================================
Transaction deserializeTx(const json& j) {
    Transaction tx;
    try {
        tx.txHash               = getString(j, "txHash",    MAX_HASH_LEN);
        tx.toAddress            = getString(j, "toAddress", MAX_ADDRESS_LEN);
        tx.value                = getUint64(j, "value");
        tx.gasLimit             = getUint64(j, "gasLimit");
        tx.maxFeePerGas         = getUint64(j, "maxFeePerGas");
        tx.maxPriorityFeePerGas = getUint64(j, "maxPriorityFeePerGas");
        tx.nonce                = getUint64(j, "nonce");
        tx.chainId              = getUint64(j, "chainId");
        tx.v                    = getUint64(j, "v");
        tx.r                    = getSigComponent(j, "r");
        tx.s                    = getSigComponent(j, "s");

        requireString(j, "data");
        tx.data = base64Decode(j.at("data").get<std::string>());

        // Issue 5: address format validation
        if (!isValidHexAddress(tx.toAddress))
            throw std::runtime_error(
                "toAddress is not a valid 20-byte hex address");

        // Issue 4: data size limit
        if (tx.data.size() > MAX_TX_DATA_BYTES)
            throw std::runtime_error(
                "Transaction data exceeds 128 KB limit");

        // Issue 3: chain ID validation
        if (tx.chainId != MEDORCOIN_CHAIN_ID)
            throw std::runtime_error(
                "Transaction chainId " + std::to_string(tx.chainId) +
                " does not match network chainId " +
                std::to_string(MEDORCOIN_CHAIN_ID));

        // Issue 2: signature verification
        // Build compact 64-byte signature from r and s components
        if (tx.r.size() != 32 || tx.s.size() != 32)
            throw std::runtime_error(
                "Transaction signature r and s must each be 32 bytes");

        unsigned char sig64[64];
        memcpy(sig64,      tx.r.data(), 32);
        memcpy(sig64 + 32, tx.s.data(), 32);

        // Extract recovery ID from v field
        int recoveryId = crypto::computeRecoveryId(tx.v, tx.chainId);
        if (recoveryId < 0)
            throw std::runtime_error(
                "Transaction v field is invalid for chainId " +
                std::to_string(tx.chainId));

        // Recompute transaction hash from contents
        unsigned char computedHash[32];
        computeTxHash(tx, computedHash);

        // Issue 1: verify declared txHash matches computed hash
        // Convert computed hash to hex for comparison
        char hexHash[65];
        for (int i = 0; i < 32; i++)
            snprintf(hexHash + i * 2, 3, "%02x", computedHash[i]);
        hexHash[64] = '\0';

        if (tx.txHash != std::string(hexHash))
            throw std::runtime_error(
                "Transaction txHash does not match computed hash of contents. "
                "Transaction may have been tampered with.");

        // Recover sender public key from signature
        unsigned char recoveredPubkey[33];
        if (!crypto::recoverPubkey(
                computedHash, sig64, recoveryId, recoveredPubkey))
            throw std::runtime_error(
                "Failed to recover public key from transaction signature. "
                "Signature is invalid.");

        // Verify signature is valid for the recovered public key
        if (!crypto::verifyHashWithPubkey(
                computedHash, recoveredPubkey, sig64))
            throw std::runtime_error(
                "Transaction ECDSA signature verification failed.");

    } catch (const json::exception& e) {
        throw std::runtime_error(
            std::string("Transaction deserialization failed: ") + e.what());
    } catch (const std::bad_alloc&) {
        throw std::runtime_error(
            "Transaction deserialization failed: out of memory. "
            "Transaction may be maliciously oversized.");
    }
    return tx;
}

// =============================================================================
// BLOCK SERIALIZATION
// =============================================================================
json serializeBlock(const Block& block) {
    json j;
    j["previousHash"] = block.previousHash;
    j["timestamp"]    = block.timestamp;
    j["hash"]         = block.hash;
    j["signature"]    = block.signature;
    j["transactions"] = json::array();
    j["transactions"].get_ref<json::array_t&>().reserve(
        block.transactions.size());
    for (const auto& tx : block.transactions)
        j["transactions"].push_back(serializeTx(tx));
    return j;
}

// =============================================================================
// BLOCK DESERIALIZATION
// Issue 6: checks for duplicate txHash and non-decreasing nonce ordering.
// =============================================================================
Block deserializeBlock(const json& j) {
    Block block;
    try {
        block.previousHash = getString(j, "previousHash", MAX_HASH_LEN);
        block.hash         = getString(j, "hash",         MAX_HASH_LEN);
        block.signature    = getString(j, "signature",    MAX_SIGNATURE_LEN);
        block.timestamp    = getUint64(j, "timestamp");

        if (!j.contains("transactions") ||
            !j.at("transactions").is_array())
            throw std::runtime_error(
                "Missing or invalid field: transactions");

        const auto& txArray = j.at("transactions");

        if (txArray.size() > MAX_TX_PER_BLOCK)
            throw std::runtime_error(
                "Block exceeds maximum of " +
                std::to_string(MAX_TX_PER_BLOCK) + " transactions");

        block.transactions.reserve(txArray.size());

        // Issue 6: track seen txHashes to reject duplicates
        std::unordered_set<std::string> seenHashes;

        for (const auto& txJson : txArray) {
            Transaction tx = deserializeTx(txJson);

            // Reject duplicate txHash within same block
            if (!seenHashes.insert(tx.txHash).second)
                throw std::runtime_error(
                    "Block contains duplicate transaction hash: " +
                    tx.txHash);

            block.transactions.push_back(std::move(tx));
        }

        // Issue 6: validate nonce ordering per sender
        // Nonces from the same sender must be strictly increasing
        std::unordered_map<std::string, uint64_t> lastNonce;
        for (const auto& tx : block.transactions) {
            auto it = lastNonce.find(tx.toAddress);
            if (it != lastNonce.end()) {
                if (tx.nonce <= it->second)
                    throw std::runtime_error(
                        "Block contains out-of-order or duplicate nonce "
                        "for address: " + tx.toAddress);
                it->second = tx.nonce;
            } else {
                lastNonce[tx.toAddress] = tx.nonce;
            }
        }

    } catch (const json::exception& e) {
        throw std::runtime_error(
            std::string("Block deserialization failed: ") + e.what());
    } catch (const std::bad_alloc&) {
        throw std::runtime_error(
            "Block deserialization failed: out of memory. "
            "Block may be maliciously oversized.");
    }
    return block;
}
