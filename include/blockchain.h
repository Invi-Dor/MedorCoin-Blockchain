#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "block.h"
#include "transaction.h"
#include "utxo.h"
#include "db/blockdb.h"
#include "db/accountdb.h"

class Blockchain {
public:
    Blockchain(const std::string &ownerAddr);

    // Chain state
    std::vector<Block> chain;
    UTXOSet utxoSet;

    // Config
    std::string ownerAddress;
    uint32_t medor;            // PoW difficulty
    uint64_t totalSupply;
    uint64_t maxSupply;

    // Persistent DBs
    BlockDB blockDB;
    AccountDB accountDB;

    // ---------------------------
    // Base fee (for gas market)
    // ---------------------------
    uint64_t getCurrentBaseFee() const;
    void setCurrentBaseFee(uint64_t fee);
    void burnBaseFees(uint64_t amount);
    void adjustBaseFee(uint64_t gasUsed, uint64_t gasLimit);

    // ---------------------------
    // Account balance helpers
    // ---------------------------
    uint64_t getBalance(const std::string &addr) const;
    void setBalance(const std::string &addr, uint64_t amount);
    void addBalance(const std::string &addr, uint64_t amount);

    // ---------------------------
    // Fork resolution
    // Adopt the longest valid chain
    // ---------------------------
    bool resolveFork(const std::vector<Block> &candidate);

    // ---------------------------
    // Block reward + mining
    // ---------------------------
    uint64_t calculateReward();
    void mineBlock(Block &block);

    // Add a block with transactions
    void addBlock(const std::string &minerAddress,
                  std::vector<Transaction> &transactions);

    // ---------------------------
    // Chain validation
    // ---------------------------
    bool validateBlock(const Block &block, const Block &previousBlock);
    bool validateChain();

    // Debug print
    void printChain() const;

private:
    uint64_t baseFeePerGas = 1;  // base fee starts at 1 unit
};
