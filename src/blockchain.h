#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "block.h"
#include "transaction.h"
#include "utxo.h"

class Blockchain {
public:
    std::vector<Block> chain;
    UTXOSet utxoSet;
    std::string ownerAddress;
    uint32_t medor;
    uint64_t totalSupply;
    uint64_t maxSupply;

    Blockchain(const std::string& ownerAddr);

    uint64_t calculateReward();
    void mineBlock(Block& block);
    void addBlock(const std::string& minerAddress, std::vector<Transaction>& transactions);
};
