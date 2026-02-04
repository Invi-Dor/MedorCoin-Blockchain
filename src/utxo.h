#ifndef UTXO_H
#define UTXO_H

#include <string>
#include <vector>
#include <cstdint>
#include "transaction.h"

struct UTXO {
    std::string txid;
    uint32_t index;
    uint64_t value;
    std::string owner;
};

class UTXOSet {
public:
    static UTXOSet& getInstance();

    void addUTXO(const UTXO& utxo);
    void removeUTXO(const std::string& txid, uint32_t index);
    std::vector<UTXO> getUTXOs(const std::string& address);

private:
    std::vector<UTXO> utxos;

    UTXOSet() = default;
};

#endif
