#include "consensus.h"
#include "crypto.h"  // doubleSHA256
#include <string>
#include <cmath>

// -------------------------------
// ProofOfWork
// -------------------------------

ProofOfWork::ProofOfWork(uint32_t difficultyTarget)
    : difficulty(difficultyTarget) {}

std::string ProofOfWork::targetString() const {
    return std::string(difficulty, '0');
}

void ProofOfWork::mineBlock(Block& block) {
    std::string target = targetString();
    block.nonce = 0;
    block.hash = doubleSHA256(block.headerToString());

    while (block.hash.substr(0, difficulty) != target) {
        block.nonce++;
        block.hash = doubleSHA256(block.headerToString());
    }
}

bool ProofOfWork::validateBlock(const Block& block) const {
    std::string target = targetString();
    std::string blockHash = doubleSHA256(block.headerToString());
    return blockHash.substr(0, difficulty) == target;
}

// -------------------------------
// Consensus
// -------------------------------

bool Consensus::validateBlock(const Block& block, const Block& previousBlock) {
    // Check PoW
    ProofOfWork powValidator(previousBlock.difficulty);
    if (!powValidator.validateBlock(block)) return false;

    // Validate block number sequence
    if (block.index != previousBlock.index + 1) return false;

    // Validate base fee
    uint64_t expectedBaseFee = computeNextBaseFee(previousBlock.baseFee, previousBlock.gasUsed, previousBlock.gasLimit);
    if (block.baseFee != expectedBaseFee) return false;

    return true;
}

bool Consensus::validateChain(const Blockchain& chain) {
    if (chain.blocks.empty()) return true;
    for (size_t i = 1; i < chain.blocks.size(); ++i) {
        if (!validateBlock(chain.blocks[i], chain.blocks[i - 1])) return false;
    }
    return true;
}

// EIP-1559 style base fee adjustment
uint64_t Consensus::computeNextBaseFee(uint64_t parentBaseFee, uint64_t gasUsed, uint64_t gasLimit) const {
    if (gasUsed == gasLimit) return parentBaseFee * 2; // simple cap
    if (gasUsed == 0) return parentBaseFee / 2;       // simple floor
    // proportional adjustment
    int64_t delta = static_cast<int64_t>(parentBaseFee) * static_cast<int64_t>(gasUsed - gasLimit / 2) / static_cast<int64_t>(gasLimit / 2);
    int64_t nextFee = static_cast<int64_t>(parentBaseFee) + delta;
    if (nextFee < 1) nextFee = 1;
    return static_cast<uint64_t>(nextFee);
}
