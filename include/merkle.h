#pragma once

#include <vector>
#include <string>
#include <array>
#include <future>
#include "transaction.h"
#include "thread_pool.h"
#include "net/serialization.h"

// Ensures binary-compatible 256-bit hash representation across the system
using UInt256 = std::array<uint8_t, 32>;

class MerkleTree {
public:
    /**
     * @brief Construct a Merkle Tree from a vector of transactions.
     * Uses ThreadPool for parallel computation on large blocks and
     * enforces deterministic ordering.
     */
    explicit MerkleTree(std::vector<Transaction> transactions, ThreadPool* pool = nullptr);

    // Get the final consensus root
    UInt256 getRoot() const noexcept { return root_; }
    std::string getRootHex() const noexcept;

    // SPV Support: Generate a proof that a transaction exists in this tree
    std::vector<UInt256> getProof(size_t index) const;

    // Static verification: Used by light clients/SPV nodes
    static bool verifyProof(const UInt256& txHash, const std::vector<UInt256>& proof, 
                           const UInt256& root, size_t index) noexcept;

    // Performance tracking
    uint64_t getComputeTimeUs() const noexcept { return computeTimeUs_; }

private:
    UInt256 root_;
    std::vector<UInt256> leaves_;
    uint64_t computeTimeUs_ = 0;

    // Iterative folding logic (thread-safe and stack-safe)
    UInt256 computeIterative(std::vector<UInt256> layer, ThreadPool* pool);
    
    // Internal binary hashing helper
    static UInt256 safeHexToBytes(const std::string& hex);
};
