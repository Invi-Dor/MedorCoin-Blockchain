#pragma once

#ifndef MEMPOOL_H
#define MEMPOOL_H

#include "transaction.h"
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>

class Mempool {
public:
    Mempool();
    ~Mempool();

    // Add transaction to mempool if not already present
    bool addTransaction(const Transaction &tx);

    // Remove by hash
    void removeTransaction(const std::string &txHash);

    // Get all mempool transactions
    std::vector<Transaction> getTransactions() const;

    // Get sorted transactions by fee (higher effective fee first)
    std::vector<Transaction> getSortedByFee(uint64_t currentBaseFee) const;

    // Check existence
    bool hasTransaction(const std::string &txHash) const;

    // Remove a set that were just confirmed
    void removeConfirmed(const std::vector<Transaction> &txs);

private:
    // In‑memory map: txHash → Transaction
    mutable std::mutex mtx;
    std::unordered_map<std::string, Transaction> txMap;

    // Optional: RocksDB for persistent mempool (uncomment if needed)
    // class MempoolDB *mempoolDB;
};

#endif // MEMPOOL_H
