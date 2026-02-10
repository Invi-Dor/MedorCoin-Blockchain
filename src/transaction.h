#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <evmc/evmc.hpp> // EVMC address types

// -------------------------------
// UTXO Transaction Structures
// -------------------------------

struct TxInput {
    std::string prevTxHash;   // Hash of transaction being spent
    uint32_t outputIndex;     // Which output is spent
    std::string signature;    // Unlocking signature
};

struct TxOutput {
    uint64_t value;           // Amount of MedorCoin
    std::string address;      // Destination address
};

// -------------------------------
// Transaction Type Enum
// -------------------------------

enum class TxType {
    Standard,        // Standard UTXO transfer
    ContractDeploy,  // Deploy a smart contract
    ContractCall     // Call a smart contract
};

// -------------------------------
// Main Transaction Class
// -------------------------------

class Transaction {
public:
    uint32_t version = 1;               // Default version
    TxType type = TxType::Standard;      // Standard by default

    // UTXO fields
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;

    // EVM fields
    std::string fromAddress;             // Sender address
    std::string toAddress;               // Contract address (empty for deploy)
    std::vector<uint8_t> data;           // Bytecode for deploy, calldata for calls

    uint64_t gasLimit = 0;               // Gas limit
    uint64_t maxFeePerGas = 0;           // Max fee user will pay
    uint64_t maxPriorityFeePerGas = 0;   // Priority tip for miner

    uint32_t lockTime = 0;               // Lock time
    std::string txHash;                  // TX identifier

    // Compute transaction ID
    void calculateHash();
};

// -------------------------------
// Transaction Processor
// -------------------------------

// Returns true if processed successfully
// minerAddress is passed so fee logic can credit miner
bool processTransaction(const Transaction& tx,
                        class Blockchain& chain,
                        const std::string& minerAddress);

// Debug output
void printTransaction(const Transaction& tx);
