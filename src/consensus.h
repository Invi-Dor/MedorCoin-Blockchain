#pragma once
#include "block.h"
#include "blockchain.h"
#include <cstdint>

class ProofOfWork {
public:
    ProofOfWork(uint32_t difficultyTarget);

    // Mine a block
    void mineBlock(Block& block);

    // Validate block PoW
    bool validateBlock(const Block& block) const;

    // Generate target string
    std::string targetString() const;

private:
    uint32_t difficulty;
};

// Consensus system for MedorCoin
class Consensus {
public:
    // Validate a single block against previous
    bool validateBlock(const Block& block, const Block& previousBlock);

    // Validate an entire chain
    bool validateChain(const Blockchain& chain);

    // Track base fee (like Ethereum EIP-1559)
    uint64_t computeNextBaseFee(uint64_t parentBaseFee, uint64_t gasUsed, uint64_t gasLimit) const;
};
