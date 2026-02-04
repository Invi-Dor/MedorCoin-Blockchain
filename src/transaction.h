#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct TxInput {
    std::string prevTxHash;
    int outputIndex;
    std::string signature;
};

struct TxOutput {
    uint64_t value;
    std::string address;
};

class Transaction {
public:
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;
    std::string txHash;

    void calculateHash();
};
