#pragma once
#include <vector>
#include <string>
#include <map>
#include "block.h"

class Blockchain {
public:
    std::vector<Block> chain;
    uint32_t medor;

    uint64_t totalSupply;
    uint64_t maxSupply;

    std::string ownerAddress;
    std::map<std::string, uint64_t> balances;

    Blockchain(std::string ownerAddr);

    Block createGenesisBlock();
    uint64_t calculateReward();
    void mineBlock(Block &block);
    void addBlock(const std::string &data, const std::string &minerAddr);
    void printChain() const;
};
