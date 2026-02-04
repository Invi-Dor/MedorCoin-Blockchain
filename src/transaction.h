#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <string>
#include <vector>
#include <cstdint>

struct TxInput {
    std::string prevTxHash;
    uint32_t outputIndex;
    std::string signature; // Simplified signature
};

struct TxOutput {
    uint64_t value;        // Amount in smallest unit (like satoshi)
    std::string recipient; // Wallet address
};

class Transaction {
public:
    std::string txid;
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;

    Transaction() = default;
    Transaction(const std::vector<TxInput>& in, const std::vector<TxOutput>& out);

    std::string computeTxID();
};

#endif
