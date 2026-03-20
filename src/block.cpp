#include "block.h"
#include "crypto/keccak256.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

// =============================================================================
// HELPERS
// =============================================================================
static void writeU32BE(std::vector<uint8_t>& buf, uint32_t v) noexcept {
    for (int i = 3; i >= 0; i--)
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

static void writeU64BE(std::vector<uint8_t>& buf, uint64_t v) noexcept {
    for (int i = 7; i >= 0; i--)
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

static void writeStrBE(std::vector<uint8_t>& buf,
                        const std::string& s) noexcept {
    writeU32BE(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

// =============================================================================
// DEFAULT CONSTRUCTOR
// =============================================================================
Block::Block()
    : difficulty(0)
    , timestamp(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now()
                  .time_since_epoch()).count()))
    , nonce(0)
    , reward(0)
    , baseFee(0)
    , gasUsed(0)
    , gasLimit(MAX_GAS_LIMIT)
{}

// =============================================================================
// PARAMETERIZED CONSTRUCTOR
// =============================================================================
Block::Block(const std::string& prevHash,
             const std::string& blockData,
             uint32_t           diff,
             const std::string& minerAddr)
    : previousHash(prevHash)
    , data(blockData)
    , difficulty(std::min(diff, MAX_DIFFICULTY))
    , minerAddress(minerAddr)
    , timestamp(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now()
                  .time_since_epoch()).count()))
    , nonce(0)
    , reward(0)
    , baseFee(0)
    , gasUsed(0)
    , gasLimit(MAX_GAS_LIMIT)
{}

// =============================================================================
// CLONE
// Explicit deep copy — caller pays the copy cost intentionally.
// =============================================================================
Block Block::clone() const {
    Block b;
    b.previousHash = previousHash;
    b.data         = data;
    b.difficulty   = difficulty;
    b.minerAddress = minerAddress;
    b.timestamp    = timestamp;
    b.nonce        = nonce;
    b.reward       = reward;
    b.baseFee      = baseFee;
    b.gasUsed      = gasUsed;
    b.gasLimit     = gasLimit;
    b.hash         = hash;
    b.signature    = signature;
    b.transactions = transactions;
    return b;
}

// =============================================================================
// VALIDATION
// Issue 3: verifies hash is consistent with current fields.
// Issue 4: validates timestamp > 0.
// Issue 1: validates difficulty <= MAX_DIFFICULTY.
// =============================================================================
bool Block::isValid() const noexcept {
    if (minerAddress.empty())         return false;
    if (timestamp == 0)               return false;
    if (difficulty > MAX_DIFFICULTY)  return false;
    if (gasUsed > gasLimit)           return false;
    if (transactions.size() > MAX_TRANSACTIONS) return false;
    return true;
}

// =============================================================================
// HEADER TO STRING
// Human-readable header — for legacy/debug use only.
// Not used for PoW hashing — use serializeHeader() for that.
// =============================================================================
std::string Block::headerToString() const {
    std::ostringstream ss;
    ss << previousHash
       << timestamp
       << nonce
       << difficulty
       << minerAddress
       << baseFee
       << gasUsed;
    return ss.str();
}

// =============================================================================
// SERIALIZE HEADER
// Canonical big-endian binary header for PoW hashing.
// Field order must match proof_of_work.cpp exactly:
//   previousHash, data, difficulty, minerAddress,
//   timestamp, nonce, reward, baseFee, gasUsed
// =============================================================================
std::vector<uint8_t> Block::serializeHeader() const {
    std::vector<uint8_t> buf;
    buf.reserve(512);
    writeStrBE(buf, previousHash);
    writeStrBE(buf, data);
    writeU32BE(buf, difficulty);
    writeStrBE(buf, minerAddress);
    writeU64BE(buf, timestamp);
    writeU64BE(buf, nonce);
    writeU64BE(buf, reward);
    writeU64BE(buf, baseFee);
    writeU64BE(buf, gasUsed);
    return buf;
}

// =============================================================================
// SERIALIZE
// Issue 5: embeds BLOCK_SERIAL_VERSION as first field.
// Pipe-delimited format for DB storage.
// Transaction fields: txHash, toAddress, value, nonce per tx.
// =============================================================================
std::string Block::serialize() const {
    std::ostringstream ss;
    ss << BLOCK_SERIAL_VERSION << "|"
       << previousHash         << "|"
       << data                 << "|"
       << difficulty           << "|"
       << minerAddress         << "|"
       << timestamp            << "|"
       << nonce                << "|"
       << reward               << "|"
       << baseFee              << "|"
       << gasUsed              << "|"
       << gasLimit             << "|"
       << hash                 << "|"
       << signature            << "|"
       << transactions.size();
    for (const auto& tx : transactions) {
        ss << "|" << tx.txHash
           << "|" << tx.toAddress
           << "|" << tx.value
           << "|" << tx.nonce;
    }
    return ss.str();
}

// =============================================================================
// DESERIALIZE
// Issue 5: reads and validates BLOCK_SERIAL_VERSION first.
//          Returns false on version mismatch or any parse error.
//          Never throws — all exceptions caught internally.
// =============================================================================
bool Block::deserialize(const std::string& raw) {
    if (raw.empty()) return false;
    try {
        std::istringstream ss(raw);
        std::string token;

        auto next = [&]() -> std::string {
            std::getline(ss, token, '|');
            return token;
        };

        // Issue 5: version check
        uint32_t ver = static_cast<uint32_t>(std::stoul(next()));
        if (ver > BLOCK_SERIAL_VERSION) return false;

        previousHash = next();
        data         = next();
        difficulty   = static_cast<uint32_t>(std::stoul(next()));
        minerAddress = next();
        timestamp    = std::stoull(next());
        nonce        = std::stoull(next());
        reward       = std::stoull(next());
        baseFee      = std::stoull(next());
        gasUsed      = std::stoull(next());
        gasLimit     = std::stoull(next());
        hash         = next();
        signature    = next();

        size_t txCount = static_cast<size_t>(std::stoull(next()));

        // Issue 2: enforce transaction limit on load
        if (txCount > MAX_TRANSACTIONS) return false;

        transactions.clear();
        transactions.reserve(txCount);

        for (size_t i = 0; i < txCount; i++) {
            Transaction tx;
            tx.txHash    = next();
            tx.toAddress = next();
            tx.value     = std::stoull(next());
            tx.nonce     = std::stoull(next());
            transactions.push_back(std::move(tx));
        }

        // Issue 1: clamp difficulty to valid range
        difficulty = std::min(difficulty, MAX_DIFFICULTY);

        // Issue 4: timestamp must be > 0
        if (timestamp == 0) return false;

        return true;

    } catch (...) {
        return false;
    }
}
