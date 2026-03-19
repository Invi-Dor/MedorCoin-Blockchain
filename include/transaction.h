#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct TxInput {
    std::string prevTxHash;
    int         outputIndex = 0;
};

// TxOutput is defined here only.
// utxo.h includes transaction.h instead of redefining TxOutput.
struct TxOutput {
    uint64_t    value   = 0;
    std::string address;
};

class Transaction {
public:
    uint64_t             chainId              = 0;
    uint64_t             nonce                = 0;
    uint64_t             maxPriorityFeePerGas = 0;
    uint64_t             maxFeePerGas         = 0;
    uint64_t             gasLimit             = 0;
    std::string          toAddress;
    uint64_t             value                = 0;
    std::vector<uint8_t> data;

    std::vector<TxInput>  inputs;
    std::vector<TxOutput> outputs;

    uint64_t                v = 0;
    std::array<uint8_t, 32> r{};
    std::array<uint8_t, 32> s{};

    std::string txHash;

    bool calculateHash();
    bool isValid() const;
};
