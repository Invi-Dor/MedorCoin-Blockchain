#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "block.h"
#include "transaction.h"
#include "utxo.h"
#include "db/blockdb.h"
#include "db/accountdb.h"

// Forward
namespace consensus { class ValidatorRegistry; }

class Blockchain {
public:
    Blockchain(const std::string &ownerAddr);

    std::vector<Block> chain;
    UTXOSet utxoSet;

    std::string ownerAddress;
    uint32_t medor;
    uint64_t totalSupply;
    uint64_t maxSupply;

    BlockDB blockDB;
    AccountDB accountDB;

    uint64_t getCurrentBaseFee() const;
    void setCurrentBaseFee(uint64_t fee);
    void burnBaseFees(uint64_t amount);
    void adjustBaseFee(uint64_t gasUsed, uint64_t gasLimit);

    uint64_t getBalance(const std::string &addr) const;
    void setBalance(const std::string &addr, uint64_t amount);
    void addBalance(const std::string &addr, uint64_t amount);

    uint64_t getNonce(const std::string &addr) const;
    bool findTransaction(const std::string &hash, Transaction &txOut) const;

    void addBlock(const std::string &minerAddress,
                  std::vector<Transaction> &transactions);

    bool validateBlock(const Block &block, const Block &previousBlock);
    bool validateChain();

    void printChain() const;

    // ------------------------
    // PoA Validator check
    // ------------------------
    bool isValidator(const std::string &addrHex) const;

private:
    uint64_t baseFeePerGas = 1;
};
