#include "consensus.h"
#include "crypto.h"  // For doubleSHA256
#include <string>

// Constructor
ProofOfWork::ProofOfWork(uint32_t difficultyTarget)
    : difficulty(difficultyTarget) {}

// Generate a target string like "0000..." based on difficulty
std::string ProofOfWork::targetString() const {
    return std::string(difficulty, '0');
}

// Mine a block (find nonce that satisfies target)
void ProofOfWork::mineBlock(Block &block) {
    std::string target = targetString();

    block.nonce = 0;
    block.hash = doubleSHA256(block.headerToString());

    while (block.hash.substr(0, difficulty) != target) {
        block.nonce++;
        block.hash = doubleSHA256(block.headerToString());
    }
}

// Validate block PoW
bool ProofOfWork::validateBlock(const Block &block) const {
    std::string target = targetString();
    std::string blockHash = doubleSHA256(block.headerToString());
    return blockHash.substr(0, difficulty) == target;
}
