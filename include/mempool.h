#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include "transaction.h"

class Mempool {
public:
    Mempool();
    ~Mempool();

    bool addTransaction(const Transaction &tx);
    void removeTransaction(const std::string &txHash);
    std::vector<Transaction> getTransactions() const;
    bool hasTransaction(const std::string &txHash) const;
    void removeConfirmed(const std::vector<Transaction> &txs);
    std::vector<Transaction> getSortedByFee(uint64_t currentBaseFee) const;

private:
    mutable std::mutex mtx;
    std::unordered_map<std::string, Transaction> txMap;
};

#endif // MEMPOOL_H
