#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <evmc/evmc.hpp> // Needed for EVM address types

// -------------------------------
// UTXO Transaction Structures
// -------------------------------

// A transaction input references a previous UTXO and includes a signature
struct TxInput {
    std::string prevTxHash;   // The hash (ID) of the transaction being spent
    uint32_t outputIndex;     // Index of the specific output in that TX
    std::string signature;    // Signature unlocking that output
};

// A transaction output defines a new UTXO — value and destination address
struct TxOutput {
    uint64_t value;           // Amount in MedorCoin units
    std::string address;      // Bitcoin‑style Base58Check address
};

// -------------------------------
// Transaction Type Enum
// -------------------------------

enum class TxType {
    Standard,        // Standard UTXO transfer
    ContractDeploy,  // EVM smart contract deployment
    ContractCall     // EVM smart contract call
};

// -------------------------------
// Main Transaction Class
// -------------------------------

class Transaction {
public:
    uint32_t version = 1;                // Transaction version (default 1)
    TxType type = TxType::Standard;       // Transaction type (standard by default)

    // Standard UTXO fields
    std::vector<TxInput> inputs;         // Input list
    std::vector<TxOutput> outputs;       // Output list

    // EVM fields
    std::string fromAddress;            // Sender address for contract
    std::string toAddress;              // Contract address (for calls) or empty for deploy
    std::vector<uint8_t> data;          // EVM bytecode (deploy) or ABI call data (call)
    uint64_t gasLimit = 0;              // Gas limit for EVM execution

    uint32_t lockTime = 0;               // Lock time (default 0)
    std::string txHash;                  // Double SHA‑256 TX identifier

    // Computes the transaction ID (Bitcoin‑style serialization + double SHA‑256)
    void calculateHash();
};
