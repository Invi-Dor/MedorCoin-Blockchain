#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

// A single output of a transaction
struct TxOutput {
    uint64_t value;
    std::string address;
};

// A UTXO entry with its originating tx and output index
struct UTXO {
    std::string txHash;
    int index;
    uint64_t value;
    std::string address;
};

class UTXOSet {
private:
    // Map key format: "txHash:index" -> UTXO
    std::unordered_map<std::string, UTXO> utxos;

    // Internal helper for generating map keys
    static std::string makeKey(const std::string& txHash, int index) {
        return txHash + ":" + std::to_string(index);
    }

public:
    // Add a new UTXO to the set
    void addUTXO(const TxOutput& output, const std::string& txHash, int index);

    // Remove a spent UTXO from the set
    void spendUTXO(const std::string& txHash, int index);

    // Compute the total balance (sum of all UTXO values) for a given address
    uint64_t getBalance(const std::string& address) const;

    // Check if a given UTXO is still unspent
    bool isUnspent(const std::string& txHash, int index) const;

    // Return a list of all UTXOs for a specific address
    std::vector<UTXO> getUTXOsFor(const std::string &address) const;
};
