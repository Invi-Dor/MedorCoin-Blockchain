#pragma once
#include <map>
#include <string>
#include "transaction.h"

struct UTXO {
    std::string txHash;
    int index;
    uint64_t value;
    std::string address;
};

class UTXOSet {
public:
    std::map<std::string, UTXO> utxos; // key = txHash:index

    void addUTXO(const TxOutput &output, const std::string &txHash, int index);
    void spendUTXO(const std::string &txHash, int index);
    uint64_t getBalance(const std::string &address);
};
