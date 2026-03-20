#pragma once

#include "transaction.h"

#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// =============================================================================
// BLOCK SERIALIZATION VERSION
// =============================================================================
static constexpr uint32_t BLOCK_SERIAL_VERSION = 2;

// =============================================================================
// BLOCK
// =============================================================================
class Block {
public:
    std::string              previousHash;
    std::string              data;
    uint32_t                 difficulty   = 0;
    std::string              minerAddress;
    uint64_t                 timestamp    = 0;
    uint64_t                 nonce        = 0;
    uint64_t                 reward       = 0;
    uint64_t                 baseFee      = 0;
    uint64_t                 gasUsed      = 0;
    uint64_t                 gasLimit     = 0;
    std::string              hash;
    std::string              signature;
    std::vector<Transaction> transactions;

    // =========================================================================
    // CONSTRUCTORS
    // Default and parameterized are allowed.
    // Copy constructor deleted — use clone() for intentional copies.
    // Move constructor allowed — zero-cost transfer of ownership.
    // =========================================================================
    Block();
    Block(const std::string& prevHash,
          const std::string& blockData,
          uint32_t           diff,
          const std::string& minerAddr);

    Block(const Block&)            = delete;
    Block& operator=(const Block&) = delete;

    Block(Block&&)            = default;
    Block& operator=(Block&&) = default;

    // Explicit clone — caller knows they are paying the copy cost
    Block clone() const;

    // =========================================================================
    // HASH LIFECYCLE
    // =========================================================================
    void clearHash() noexcept { hash.clear(); signature.clear(); }
    bool hasHash()   const noexcept { return !hash.empty(); }

    // =========================================================================
    // VALIDATION
    // =========================================================================
    bool isValid() const noexcept;

    // =========================================================================
    // SERIALIZATION
    // =========================================================================
    std::string          headerToString()  const;
    std::vector<uint8_t> serializeHeader() const;
    std::string          serialize()       const;
    bool                 deserialize(const std::string& raw);

    // =========================================================================
    // LIMITS
    // =========================================================================
    static constexpr size_t   MAX_TRANSACTIONS = 10000;
    static constexpr uint32_t MAX_DIFFICULTY   = 64;
    static constexpr uint64_t MAX_GAS_LIMIT    = 30'000'000;
};
