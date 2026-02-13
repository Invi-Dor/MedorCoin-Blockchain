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
    uint32_t outputIndex;      // Which output is spent
    // In a real UTXO chain this would include unlocking script/signature
    std::string signature;    
};

struct TxOutput {
    uint64_t value;            // Amount of MedorCoin in smallest unit
    std::string address;       // Destination address
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
    uint32_t version = 1;      // Protocol version
    TxType type = TxType::Standard;

    // UTXO fields
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;

    // EVM transaction fields
    // Nonce prevents replay attacks and orders transactions per address
    uint64_t nonce = 0;                 

    // Sender & recipient Ethereum‑style addresses (20 bytes)
    std::array<uint8_t,20> fromAddress;
    std::array<uint8_t,20> toAddress;

    // Value to transfer (in base unit, e.g., WEI‑like)
    uint64_t value = 0;

    // EVM execution input code or calldata
    std::vector<uint8_t> data;

    // Gas & fee parameters (EIP‑1559 dynamic fee model)
    uint64_t gasLimit = 0;              // Max gas for execution
    uint64_t maxFeePerGas = 0;          // Fee cap per gas
    uint64_t maxPriorityFeePerGas = 0;  // Miner tip per gas

    // Signature fields for EVM tx
    // After signing, these will be filled
    std::array<uint8_t,32> r = {};
    std::array<uint8_t,32> s = {};
    uint8_t v = 0;

    // General fields
    uint32_t lockTime = 0;
    std::string txHash;                // Calculated transaction identifier

    // Compute transaction ID or hash
    void calculateHash();
};

// -------------------------------
// Transaction Processing
// -------------------------------

// Processes a transaction on the chain.
// minerAddress is passed so fees can be credited appropriately.
bool processTransaction(const Transaction& tx,
                        class Blockchain& chain,
                        const std::string& minerAddress);

// Print human‑readable transaction info
void printTransaction(const Transaction& tx);
