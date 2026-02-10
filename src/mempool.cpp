#include "mempool.h"

// Add transaction to mempool if not already present
bool Mempool::addTransaction(const Transaction &tx) {
    std::lock_guard<std::mutex> lock(mtx);

    const std::string &hash = tx.txHash;
    if (txMap.find(hash) != txMap.end()) {
        return false;
    }

    txMap[hash] = tx;
    return true;
}

// Remove a transaction using its hash
void Mempool::removeTransaction(const std::string &txHash) {
    std::lock_guard<std::mutex> lock(mtx);
    txMap.erase(txHash);
}

// Return a vector of all transactions in the mempool
std::vector<Transaction> Mempool::getTransactions() {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<Transaction> txs;
    txs.reserve(txMap.size());

    for (auto &kv : txMap) {
        txs.push_back(kv.second);
    }
    return txs;
}

// Check if a transaction exists by its hash
bool Mempool::hasTransaction(const std::string &txHash) {
    std::lock_guard<std::mutex> lock(mtx);
    return txMap.find(txHash) != txMap.end();
}

// Remove transactions that were included in a block
void Mempool::removeConfirmed(const std::vector<Transaction> &txs) {
    std::lock_guard<std::mutex> lock(mtx);
    for (const auto &tx : txs) {
        txMap.erase(tx.txHash);
    }
}
