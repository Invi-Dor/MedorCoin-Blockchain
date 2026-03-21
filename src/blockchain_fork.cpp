#include "blockchain_fork.h"

#include <iostream>

bool resolveLongestChain(const std::vector<Block> &candidateChain,
                          std::vector<Block>        &currentChain) noexcept
{
    // ------------------------------------------------------------------
    // Guard: both chains must be non-empty
    // ------------------------------------------------------------------
    if (candidateChain.empty() || currentChain.empty()) {
        std::cerr << "[ForkResolution] One or both chains are empty -- rejected\n";
        return false;
    }

    // ------------------------------------------------------------------
    // Guard: genesis blocks must match
    // Both chains must share the same origin. If they don't, this is a
    // completely different chain, not a fork.
    // ------------------------------------------------------------------
    if (candidateChain.front().hash != currentChain.front().hash) {
        std::cerr << "[ForkResolution] Genesis hash mismatch -- rejected\n";
        return false;
    }

    // ------------------------------------------------------------------
    // Longest-chain rule
    // Only replace if candidate is strictly longer. Equal length keeps
    // the current chain (first-seen rule).
    // ------------------------------------------------------------------
    if (candidateChain.size() <= currentChain.size()) {
        std::cerr << "[ForkResolution] Candidate not longer than current ("
                  << candidateChain.size() << " <= " << currentChain.size()
                  << ") -- keeping current chain\n";
        return false;
    }

    // ------------------------------------------------------------------
    // Structural validation -- O(n) with early exit on first failure.
    //
    // For mainnet-scale chains with millions of blocks, consider:
    //   1. A hash index (unordered_map<hash, Block*>) for O(1) lookup
    //   2. Checkpoint validation -- only validate blocks after the last
    //      known good checkpoint rather than the full candidate
    //   3. DB-backed block storage so only block headers are loaded into
    //      memory during validation, not full block data
    //
    // currentChain itself is already backed by BlockDB (see blockchain.cpp
    // and blockdb.h). This validation pass operates on the candidate only.
    // ------------------------------------------------------------------
    for (size_t i = 0; i < candidateChain.size(); ++i) {
        const Block &b = candidateChain[i];

        if (b.hash.empty()) {
            std::cerr << "[ForkResolution] Block " << i
                      << " has empty hash -- rejected\n";
            return false;
        }

        if (b.timestamp == 0) {
            std::cerr << "[ForkResolution] Block " << i
                      << " has zero timestamp -- rejected\n";
            return false;
        }

        if (i > 0 && b.previousHash != candidateChain[i - 1].hash) {
            std::cerr << "[ForkResolution] Block " << i
                      << " previousHash mismatch -- rejected\n";
            return false;
        }
    }

    // ------------------------------------------------------------------
    // Build replacement vector via clone() before touching currentChain.
    // Block's copy constructor is deleted -- clone() is the only valid
    // explicit copy path. We build the full replacement first so that
    // if any clone() fails, currentChain is left completely untouched.
    //
    // Memory note: for very large chains, this temporarily holds two full
    // chain copies in memory. DB-backed pruning in BlockDB mitigates this
    // by keeping only recent blocks in the vector at any time.
    // ------------------------------------------------------------------
    std::vector<Block> replacement;
    replacement.reserve(candidateChain.size());
    for (const auto &b : candidateChain)
        replacement.push_back(b.clone());

    // ------------------------------------------------------------------
    // Atomic swap -- currentChain is replaced in a single move operation.
    // The caller (Blockchain::resolveFork) holds a unique_lock on rwMutex_
    // before calling this function, so this swap is fully thread-safe
    // under the existing Blockchain mutex without any additional locking
    // here. Adding a second mutex here would cause a deadlock.
    // ------------------------------------------------------------------
    currentChain = std::move(replacement);

    std::cout << "[ForkResolution] Chain replaced -- new height="
              << currentChain.size() - 1 << "\n";
    return true;
}
