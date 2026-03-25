#include "merkle.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cmath>

/**
 * MEDORCOIN INDUSTRIAL MERKLE ENGINE
 * - Iterative Folding: Prevents Stack Overflow on large-scale blocks.
 * - Parallelization: Uses your ThreadPool for O(n) layer folding.
 * - Endianness: Little-Endian binary concatenation for PoW parity.
 * - SPV Support: Full Proof generation and verification.
 */

MerkleTree::MerkleTree(std::vector<Transaction> transactions, ThreadPool* pool) {
    auto start = std::chrono::high_resolution_clock::now();

    if (transactions.empty()) {
        root_.fill(0);
        return;
    }

    // Fix #7: Consensus Determinism (BIP69-style sorting)
    std::sort(transactions.begin(), transactions.end(), [](const Transaction& a, const Transaction& b) {
        return a.txHash < b.txHash;
    });

    // Fix #5: Binary Extraction and Hex Validation
    leaves_.reserve(transactions.size());
    for (const auto& tx : transactions) {
        try {
            leaves_.push_back(safeHexToBytes(tx.txHash));
        } catch (const SerializationError& e) {
            // Fix #13: Integration with your Serialization Error System
            throw SerializationError(SerializationErrorCode::HashMismatch, 
                "Merkle Construction Aborted: " + std::string(e.what()));
        }
    }

    // Fix #6: Iterative folding (Stack-safe for high-scale production)
    root_ = computeIterative(leaves_, pool);

    auto end = std::chrono::high_resolution_clock::now();
    computeTimeUs_ = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

UInt256 MerkleTree::computeIterative(std::vector<UInt256> layer, ThreadPool* pool) {
    while (layer.size() > 1) {
        size_t nextSize = (layer.size() + 1) / 2;
        std::vector<UInt256> nextLayer(nextSize);

        // Fix #10: Multi-threaded folding using your ThreadPool
        if (pool && layer.size() > 1024) {
            std::vector<std::future<void>> futures;
            futures.reserve(nextSize);

            for (size_t i = 0; i < layer.size(); i += 2) {
                futures.push_back(pool->submit([&layer, &nextLayer, i]() {
                    const UInt256& left = layer[i];
                    const UInt256& right = (i + 1 < layer.size()) ? layer[i + 1] : left;
                    
                    // Fix #2: Little-Endian Binary Concatenation
                    alignas(16) uint8_t buffer[64];
                    std::memcpy(buffer, left.data(), 32);
                    std::memcpy(buffer + 32, right.data(), 32);

                    uint8_t mid[32];
                    // Fix #3: Double-SHA256 Binary Passes
                    crypto::doubleSHA256(buffer, 64, mid);
                    crypto::doubleSHA256(mid, 32, nextLayer[i / 2].data());
                }));
            }
            for (auto& f : futures) if (f.valid()) f.wait();
        } else {
            // Serial optimized path for smaller blocks
            for (size_t i = 0; i < layer.size(); i += 2) {
                const UInt256& left = layer[i];
                const UInt256& right = (i + 1 < layer.size()) ? layer[i + 1] : left;
                
                alignas(16) uint8_t buffer[64];
                std::memcpy(buffer, left.data(), 32);
                std::memcpy(buffer + 32, right.data(), 32);

                uint8_t mid[32];
                crypto::doubleSHA256(buffer, 64, mid);
                crypto::doubleSHA256(mid, 32, nextLayer[i / 2].data());
            }
        }
        layer = std::move(nextLayer);
    }
    return layer[0];
}

/**
 * Fix #4: SPV Proof Generation
 * Returns the list of hashes required to reconstruct the root from a leaf.
 */
std::vector<UInt256> MerkleTree::getProof(size_t index) const {
    if (index >= leaves_.size()) {
        throw SerializationError(SerializationErrorCode::LengthExceeded, "Proof index OOB");
    }

    std::vector<UInt256> proof;
    std::vector<UInt256> currentLevel = leaves_;

    while (currentLevel.size() > 1) {
        size_t pairIndex = (index % 2 == 0) ? index + 1 : index - 1;
        
        if (pairIndex < currentLevel.size()) {
            proof.push_back(currentLevel[pairIndex]);
        } else {
            // Fix: Bitcoin-style duplication of the last node in odd layers
            proof.push_back(currentLevel[index]);
        }

        index /= 2;
        std::vector<UInt256> nextLevel;
        for (size_t i = 0; i < currentLevel.size(); i += 2) {
            const UInt256& left = currentLevel[i];
            const UInt256& right = (i + 1 < currentLevel.size()) ? currentLevel[i + 1] : left;
            
            UInt256 combined;
            alignas(16) uint8_t buf[64];
            std::memcpy(buf, left.data(), 32);
            std::memcpy(buf + 32, right.data(), 32);
            uint8_t mid[32];
            crypto::doubleSHA256(buf, 64, mid);
            crypto::doubleSHA256(mid, 32, combined.data());
            nextLayer.push_back(combined);
        }
        currentLevel = std::move(nextLevel);
    }
    return proof;
}

/**
 * Fix #4: SPV Verification
 * Pure function to verify a leaf against a root using a proof.
 */
bool MerkleTree::verifyProof(const UInt256& txHash, const std::vector<UInt256>& proof, 
                             const UInt256& root, size_t index) noexcept {
    UInt256 current = txHash;
    for (const auto& sibling : proof) {
        alignas(16) uint8_t buf[64];
        if (index % 2 == 0) {
            std::memcpy(buf, current.data(), 32);
            std::memcpy(buf + 32, sibling.data(), 32);
        } else {
            std::memcpy(buf, sibling.data(), 32);
            std::memcpy(buf + 32, current.data(), 32);
        }
        uint8_t mid[32];
        crypto::doubleSHA256(buf, 64, mid);
        crypto::doubleSHA256(mid, 32, current.data());
        index /= 2;
    }
    return std::memcmp(current.data(), root.data(), 32) == 0;
}

UInt256 MerkleTree::safeHexToBytes(const std::string& hex) {
    if (hex.length() != 64) 
        throw SerializationError(SerializationErrorCode::LengthExceeded, "Hash must be 64 hex chars");
    
    UInt256 out;
    for (size_t i = 0; i < 32; ++i) {
        auto decode = [](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            throw SerializationError(SerializationErrorCode::TypeMismatch, "Invalid hex character");
        };
        out[i] = (decode(hex[i * 2]) << 4) | decode(hex[i * 2 + 1]);
    }
    return out;
}
