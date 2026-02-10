#include "blockchain.h"
#include "block.h"
#include "transaction.h"
#include "utxo.h"
#include "consensus.h"
#include <ctime>
#include <cstdint>
#include <vector>
#include <string>
#include <iostream>

// -------------------------------
// Constructor
// -------------------------------

Blockchain::Blockchain(const std::string &ownerAddr)
    : blockDB(), accountDB("data/medorcoin_accounts")
{
    ownerAddress = ownerAddr;
    medor = 0x1e00ffff;
    totalSupply = 0;
    maxSupply = 50000000;

    // Open LevelDB for block data
    if (!blockDB.open("data/medorcoin_blocks"))
        std::cerr << "[BlockDB] Failed to open blocks DB" << std::endl;

    // Load blocks from DB
    leveldb::Iterator *it = blockDB.db->NewIterator(leveldb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        Block b;
        std::string key = it->key().ToString();
        if (blockDB.readBlock(key, b)) {
            chain.push_back(b);
        }
    }
    delete it;

    // If empty, create genesis block
    if (chain.empty()) {
        std::vector<Transaction> noTxs;
        addBlock(ownerAddress, noTxs);
    }
}

// -------------------------------
// Balance / Account State
// -------------------------------

uint64_t Blockchain::getBalance(const std::string &addr) const {
    std::string stored;
    if (accountDB.get("bal:" + addr, stored)) {
        try {
            return std::stoull(stored);
        } catch (...) {
            return 0ULL;
        }
    }
    return 0ULL;
}

void Blockchain::setBalance(const std::string &addr, uint64_t amount) {
    accountDB.put("bal:" + addr, std::to_string(amount));
}

void Blockchain::addBalance(const std::string &addr, uint64_t amount) {
    uint64_t current = getBalance(addr);
    setBalance(addr, current + amount);
}

// -------------------------------
// Base Fee Support
// -------------------------------

uint64_t Blockchain::getCurrentBaseFee() const {
    return baseFeePerGas;
}

void Blockchain::setCurrentBaseFee(uint64_t fee) {
    baseFeePerGas = (fee < 1 ? 1 : fee);
}

void Blockchain::burnBaseFees(uint64_t amount) {
    // Credit the fee to a treasury address instead of destroying
    const std::string treasuryAddr = "medor_treasury";
    addBalance(treasuryAddr, amount);
    std::cout << "[Fee] Treasury credited " << amount << std::endl;
}

void Blockchain::adjustBaseFee(uint64_t gasUsed, uint64_t gasLimit) {
    if (gasLimit == 0) return;
    if (gasUsed * 2 > gasLimit) {
        baseFeePerGas += baseFeePerGas / 8;  // increase
    } else {
        baseFeePerGas = (baseFeePerGas > 1 ? baseFeePerGas - baseFeePerGas / 8 : 1);
    }
}

// -------------------------------
// Reward + Mining
// -------------------------------

uint64_t Blockchain::calculateReward() {
    time_t now = time(nullptr);
    time_t genesisTime = chain.empty() ? now : chain.front().timestamp;
    double months = difftime(now, genesisTime) / (60 * 60 * 24 * 30.0);
    return (months <= 2.0) ? 55 : 30;
}

void Blockchain::mineBlock(Block &block) {
    ProofOfWork pow(medor);
    pow.mineBlock(block);
}

// -------------------------------
// Add Block (PoW + fees + UTXO)
// -------------------------------

void Blockchain::addBlock(const std::string &minerAddr,
                          std::vector<Transaction> &transactions)
{
    // Create reward coinbase
    uint64_t reward = calculateReward();
    if (totalSupply + reward > maxSupply)
        reward = maxSupply - totalSupply;

    Transaction coinbaseTx;
    TxOutput mo, oo;
    mo.value = (reward * 90) / 100;
    mo.address = minerAddr;
    oo.value = reward - mo.value;
    oo.address = ownerAddress;
    coinbaseTx.outputs.push_back(mo);
    coinbaseTx.outputs.push_back(oo);
    coinbaseTx.calculateHash();
    transactions.insert(transactions.begin(), coinbaseTx);

    // Build block
    Block newBlock;
    if (!chain.empty())
        newBlock = Block(chain.back().hash, "MedorCoin Block", medor, minerAddr);
    else
        newBlock = Block("", "Genesis Block", medor, minerAddr);

    newBlock.timestamp = time(nullptr);
    newBlock.reward = reward;
    newBlock.transactions = transactions;
    newBlock.baseFee = baseFeePerGas;

    // Mine the block
    mineBlock(newBlock);

    // Validate PoW
    if (!ProofOfWork(medor).validateBlock(newBlock)) {
        std::cerr << "[Blockchain] Invalid PoW" << std::endl;
        return;
    }

    // Push to in‑memory chain
    chain.push_back(newBlock);

    // Persist to LevelDB
    if (!blockDB.writeBlock(newBlock))
        std::cerr << "[Blockchain] Failed to write block" << std::endl;

    // Update UTXO
    for (auto &tx : newBlock.transactions) {
        for (auto &in : tx.inputs)
            utxoSet.spendUTXO(in.prevTxHash, in.outputIndex);
        for (size_t i = 0; i < tx.outputs.size(); ++i)
            utxoSet.addUTXO(tx.outputs[i], tx.txHash, static_cast<int>(i));
    }

    // Update supply
    totalSupply += reward;

    // Adjust base fee
    adjustBaseFee(newBlock.gasUsed, /* assume a gas target */ 1000000);

    std::cout << "[Blockchain] Block added: " << newBlock.hash
              << " | BaseFee: " << newBlock.baseFee
              << " | Reward: " << reward << std::endl;
}

// -------------------------------
// Chain Validation
// -------------------------------

bool Blockchain::validateBlock(const Block &block, const Block &prevBlock) {
    if (block.previousHash != prevBlock.hash) return false;
    if (block.timestamp <= prevBlock.timestamp) return false;
    if (block.baseFee < 1) return false;
    return true;
}

bool Blockchain::validateChain() {
    for (size_t i = 1; i < chain.size(); ++i)
        if (!validateBlock(chain[i], chain[i - 1]))
            return false;
    return true;
}

// -------------------------------
// Debug print
// -------------------------------

void Blockchain::printChain() const {
    std::cout << "------ MedorCoin Blockchain ------\n";
    for (size_t i = 0; i < chain.size(); ++i) {
        const Block &b = chain[i];
        std::cout << "Block " << i
                  << " | Hash: " << b.hash
                  << " | Prev: " << b.previousHash
                  << " | BaseFee: " << b.baseFee
                  << " | GasUsed: " << b.gasUsed
                  << " | TXCount: " << b.transactions.size()
                  << std::endl;
    }
}
