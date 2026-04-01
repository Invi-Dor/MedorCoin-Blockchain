#include "miner.h"
#include "blockchain.h"
#include "mempool.h"
#include "transaction.h"

#include <iostream>
#include <vector>

// =============================================================================
// mineMedor
// Mine a block with only the coinbase reward and no mempool transactions.
// =============================================================================
void Miner::mineMedor(Blockchain        &chain,
                       const std::string &minerAddress)
{
    std::cout << "[Miner] Starting mining with miner address: "
              << minerAddress << "\n";

    std::vector<Transaction> txs;
    chain.addBlock(minerAddress, txs);

    std::cout << "[Miner] Block mined and added to chain.\n";
}

// =============================================================================
// mineWithMempool
// Mine a block including valid transactions from the mempool.
// =============================================================================
void Miner::mineWithMempool(Blockchain        &chain,
                             const std::string &minerAddress,
                             Mempool           &mempool,
                             uint64_t           baseFee)
{
    std::cout << "[Miner] Mining with mempool transactions.\n";

    // getTransactions() returns all current mempool txs -- not static
    std::vector<Transaction> pendingTxs = mempool.getTransactions();

    std::vector<Transaction> validTxs;
    validTxs.reserve(pendingTxs.size());

    for (auto &tx : pendingTxs) {
        tx.calculateHash();
        // Only include txs that meet the current base fee
        if (Mempool::effectiveFee(tx, baseFee) >= baseFee) {
            validTxs.push_back(tx);
        } else {
            std::cout << "[Miner] Skipping low-fee TX: "
                      << tx.txHash << "\n";
        }
    }

    chain.addBlock(minerAddress, validTxs);

    std::cout << "[Miner] Block mined with "
              << validTxs.size() << " transactions.\n";

    // removeConfirmed takes a vector<Transaction> and an instance
    mempool.removeConfirmed(validTxs);
}
