#pragma once

#ifndef MEMPOOL_H
#define MEMPOOL_H

#include "transaction.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>

class Mempool {
public:
    Mempool() = default;
    ~Mempool() = default;

    // Add a transaction to the pool if not already present
    bool addTransaction(const Transaction &tx);

    // Remove a transaction by its hash
    void removeTransaction(const std::string &txHash);

    // Get all transactions in the mempool
    std::vector<Transaction> getTransactions();

    // Check if a transaction exists in the mempool
    bool hasTransaction(const std::string &txHash);

    // Remove a set of confirmed transactions
    void removeConfirmed(const std::vector<Transaction> &txs);

private:
    std::unordered_map<std::string, Transaction> txMap;
    std::mutex mtx;
};

#endif // MEMPOOL_H
