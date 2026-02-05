#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct TxInput {
    std::string prevTxHash;
    int outputIndex;
    std::string signature; // wallet signature
};

struct TxOutput {
    uint64_t value;
    std::string address;
};

class Transaction {
public:
    std::string txHash;
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;

    void calculateHash() {
        std::string concat;
        for (auto& out : outputs)
            concat += out.address + std::to_string(out.value);
        for (auto& in : inputs)
            concat += in.prevTxHash + std::to_string(in.outputIndex) + in.signature;
        // Simple SHA256 placeholder
        txHash = "tx" + std::to_string(rand()); 
    }
};
