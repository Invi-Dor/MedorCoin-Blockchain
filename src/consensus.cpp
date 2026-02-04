#include "consensus.h"
#include "crypto.h"
#include <iostream>
#include <sstream>
#include <iomanip>

bool Consensus::validateBlock(const Block& block, const Block& previousBlock) {
    if (block.previousHash != previousBlock.hash) {
        std::cerr << "Invalid previous hash." << std::endl;
        return false;
    }

    std::string target = medorToTarget(block.medor);
    std::string hashCheck = doubleSHA256(block.headerToString());

    if (hashCheck.substr(0, target.size()) > target) {
        std::cerr << "Block does not meet proof-of-work." << std::endl;
        return false;
    }

    return true;
}

bool Consensus::validateChain(const Blockchain& chain) {
    for (size_t i = 1; i < chain.chain.size(); i++) {
        if (!validateBlock(chain.chain[i], chain.chain[i - 1])) {
            std::cerr << "Blockchain validation failed at block " << i << std::endl;
            return false;
        }
    }
    return true;
}
