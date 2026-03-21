#include "blockchain_fork.h"
#include <iostream>

bool resolveLongestChain(const std::vector<Block> &candidateChain,
                          std::vector<Block>        &currentChain) noexcept
{
    if (candidateChain.empty() || currentChain.empty()) {
        std::cerr << "[ForkResolution] One or both chains are empty -- rejected\n";
        return false;
    }
    if (candidateChain.front().hash != currentChain.front().hash) {
        std::cerr << "[ForkResolution] Genesis hash mismatch -- rejected\n";
        return false;
    }
    if (candidateChain.size() <= currentChain.size()) {
        std::cerr << "[ForkResolution] Candidate not longer than current -- keeping current\n";
        return false;
    }
    for (size_t i = 0; i < candidateChain.size(); ++i) {
        const Block &b = candidateChain[i];
        if (b.hash.empty()) {
            std::cerr << "[ForkResolution] Block " << i << " has empty hash -- rejected\n";
            return false;
        }
        if (b.timestamp == 0) {
            std::cerr << "[ForkResolution] Block " << i << " has zero timestamp -- rejected\n";
            return false;
        }
        if (i > 0 && b.previousHash != candidateChain[i - 1].hash) {
            std::cerr << "[ForkResolution] Block " << i << " previousHash mismatch -- rejected\n";
            return false;
        }
    }
    std::vector<Block> replacement;
    replacement.reserve(candidateChain.size());
    for (const auto &b : candidateChain)
        replacement.push_back(b.clone());
    currentChain = std::move(replacement);
    std::cout << "[ForkResolution] Chain replaced -- new height="
              << currentChain.size() - 1 << "\n";
    return true;
}
