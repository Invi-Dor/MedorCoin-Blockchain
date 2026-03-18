#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct TxInput {
    std::string prevTxHash;
    int         outputIndex = 0;
};

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

    // UTXO-style inputs and outputs
    std::vector<TxInput>  inputs;
    std::vector<TxOutput> outputs;

    // Signature fields
    uint64_t                v = 0;
    std::array<uint8_t, 32> r{};
    std::array<uint8_t, 32> s{};

    // Computed transaction hash
    std::string txHash;

    // Returns true on success, false on failure
    bool calculateHash();

    // Validates the transaction fields are internally consistent
    bool isValid() const;
};
