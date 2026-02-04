#ifndef WALLET_H
#define WALLET_H

#include <string>
#include "crypto.h"
#include "transaction.h"

class Wallet {
public:
    std::string privateKey;
    std::string publicKey;
    std::string address;

    Wallet();

    Transaction createTransaction(const std::string& recipient, uint64_t amount);
};

#endif
