#include "block_builder.h"
#include <iostream>

BlockBuilder::BlockBuilder(Blockchain &chain_, Mempool &pool_)
    : chain(chain_), mempool(pool_) { }

std::vector<std::string> BlockBuilder::buildBlock(const std::string &minerAddress) {
    std::vector<std::string> included;

    // 1) Get current base fee from chain (e.g., EIP‑1559 state)
    uint64_t baseFee = chain.getCurrentBaseFee();

    // 2) Fetch sorted mempool txs (highest tip/fee first)
    auto sorted = mempool.getSortedByFee(baseFee);

    // 3) Iterate and execute, gather successful txs
    for (auto &tx : sorted) {
        if (executeAndApply(tx, minerAddress)) {
            included.push_back(tx.txHash);
        }
    }

    // 4) Remove included transactions from mempool
    mempool.removeConfirmed(chain.getTransactions(included));

    return included;
}

bool BlockBuilder::executeAndApply(const Transaction &tx, const std::string &minerAddress) {
    // Check signature validity again before execution
    if (!chain.verifyTransactionSignature(tx)) {
        std::cerr << "[BlockBuilder] Signature invalid for tx " << tx.txHash << std::endl;
        return false;
    }

    // Check balances & nonces
    uint64_t senderBal = chain.getBalance(tx.fromAddress);
    uint64_t cost = tx.value + (tx.gasLimit * tx.maxFeePerGas);
    if (senderBal < cost) {
        std::cerr << "[BlockBuilder] Insufficient balance for tx " << tx.txHash << std::endl;
        return false;
    }

    // Execute the transaction
    bool success = chain.executeTransaction(tx, minerAddress);

    if (!success) {
        std::cerr << "[BlockBuilder] Execution failed for tx " << tx.txHash << std::endl;
        return false;
    }

    // Successful execution
    return true;
}
