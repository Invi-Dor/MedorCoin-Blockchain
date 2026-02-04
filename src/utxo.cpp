#include "utxo.h"

void UTXOSet::addUTXO(const TxOutput& output,
                      const std::string& txHash,
                      int index) {
    std::string key = txHash + ":" + std::to_string(index);
    utxos[key] = {txHash, index, output.value, output.address};
}

void UTXOSet::spendUTXO(const std::string& txHash,
                        int index) {
    std::string key = txHash + ":" + std::to_string(index);
    utxos.erase(key);
}

uint64_t UTXOSet::getBalance(const std::string& address) const {
    uint64_t balance = 0;
    for (const auto& pair : utxos) {
        if (pair.second.address == address)
            balance += pair.second.value;
    }
    return balance;
}
