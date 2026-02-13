#pragma once

#include "blockchain.h"
#include "mempool.h"
#include <vector>
#include <string>

/**
 * Build a new block:
 * - Pull sorted transactions from mempool
 * - Execute each tx on the EVM
 * - Update balances, gas, state
 * - Add successful txs to block
 * - Remove them from mempool
 */
class BlockBuilder {
public:
    BlockBuilder(Blockchain &chain, Mempool &pool);

    // Build next block and return list of included transaction hashes
    std::vector<std::string> buildBlock(const std::string &minerAddress);

private:
    Blockchain &chain;
    Mempool &mempool;

    // Execute single transaction and update state
    bool executeAndApply(const Transaction &tx, const std::string &minerAddress);
};
