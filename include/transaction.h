#pragma once

#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <string>

class Transaction {
public:
    Transaction() = default;

    Transaction(const std::string &from,
                const std::string &to,
                double amount,
                const std::string &hash)
        : sender(from), recipient(to), amount(amount), hashValue(hash) {}

    // Returns a unique identifier/hash for this transaction
    std::string getHash() const {
        return hashValue;
    }

private:
    std::string sender;
    std::string recipient;
    double amount = 0.0;
    std::string hashValue;
};

#endif // TRANSACTION_H
