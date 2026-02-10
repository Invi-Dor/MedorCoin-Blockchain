#include "blockchain.h"
#include "block.h"
#include "transaction.h"
#include "utxo.h"
#include "consensus.h"
#include "db/blockdb.h"  // LevelDB storage
#include <ctime>
#include <cstdint>
#include <vector>
#include <string>
#include <iostream>

using std::string;
using std::vector;

// -------------------------------
// Constructor
// -------------------------------

Blockchain::Blockchain(const string &ownerAddr)
    : blockDB()
{
    ownerAddress = ownerAddr;
    medor = 0x1e00ffff;
    totalSupply = 0;
    maxSupply = 50000000;

    // open LevelDB
    if (!blockDB.open("data/medorcoin_blocks"))
        std::cerr << "Error: Could not open LevelDB storage" << std::endl;

    // load blocks
    leveldb::Iterator *it = blockDB.db->NewIterator(leveldb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        Block b;
        string key = it->key().ToString();
        if (blockDB.readBlock(key, b)) {
            chain.push_back(b);
        }
    }
    delete it;

    // genesis
    if (chain.empty()) {
        vector<Transaction> noTxs;
        addBlock(ownerAddress, noTxs);
    }
}

// -------------------------------
// Balance API
// -------------------------------

uint64_t Blockchain::getBalance(const string &addr) const {
    auto it = balanceMap.find(addr);
    if (it != balanceMap.end())
        return it->second;
    return 0;
}

void Blockchain::setBalance(const string &addr, uint64_t amt) {
    balanceMap[addr] = amt;
    // TODO: persist in DB if desired
}

void Blockchain::addBalance(const string &addr, uint64_t amt) {
    uint64_t cur = getBalance(addr);
    setBalance(addr, cur + amt);
}

// -------------------------------
// Base Fee API
// -------------------------------

uint64_t Blockchain::getCurrentBaseFee() const {
    return baseFeePerGas;
}

void Blockchain::setCurrentBaseFee(uint64_t fee) {
    baseFeePerGas = fee;
}

void Blockchain::burnBaseFees(uint64_t amount) {
    std::cout << "[BASE FEE] Burned " << amount << " MedorCoin units\n";
    // Optionally adjust totalSupply or send to treasury
}

// Adjust baseFee using a simple rule (can be more advanced)
void Blockchain::adjustBaseFee(uint64_t gasUsed, uint64_t gasLimit) {
    if (gasUsed > gasLimit) return;
    if (gasUsed * 2 > gasLimit) {
        baseFeePerGas += baseFeePerGas / 8; // if >50% utilized, raise
    }
    else {
        baseFeePerGas -= baseFeePerGas / 8; // if <50%, lower
        if (baseFeePerGas < 1) baseFeePerGas = 1;
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
// Add block with tx + fees
// -------------------------------

void Blockchain::addBlock(const string &minerAddr,
                          vector<Transaction> &transactions)
{
    // 1. Create block reward
    uint64_t reward = calculateReward();
    if (totalSupply + reward > maxSupply)
        reward = maxSupply - totalSupply;

    // 2. Coinbase distribution
    Transaction coinbaseTx;
    TxOutput mOut, oOut;
    mOut.value = (reward * 90) / 100;
    mOut.address = minerAddr;
    oOut.value = reward - mOut.value;
    oOut.address = ownerAddress;

    coinbaseTx.outputs.push_back(mOut);
    coinbaseTx.outputs.push_back(oOut);
    coinbaseTx.calculateHash();
    transactions.insert(transactions.begin(), coinbaseTx);

    // 3. Create the block
    Block newBlock;
    if (!chain.empty())
        newBlock = Block(chain.back().hash, "MedorCoin Block", medor, minerAddr);
    else
        newBlock = Block("", "Genesis Block", medor, minerAddr);

    newBlock.timestamp = time(nullptr);
    newBlock.reward = reward;
    newBlock.transactions = transactions;
    newBlock.baseFee = baseFeePerGas;

    // 4. Mine (PoW)
    mineBlock(newBlock);

    // 5. Validate PoW
    if (!ProofOfWork(medor).validateBlock(newBlock)) {
        std::cerr << "Error: Invalid block PoW!" << std::endl;
        return;
    }

    // 6. Persist chain
    chain.push_back(newBlock);
    if (!blockDB.writeBlock(newBlock))
        std::cerr << "Warning: Failed writing block" << std::endl;

    // 7. Update UTXO + balances
    for (auto &tx : newBlock.transactions) {
        for (auto &in : tx.inputs)
            utxoSet.spendUTXO(in.prevTxHash, in.outputIndex);

        for (size_t i = 0; i < tx.outputs.size(); ++i)
            utxoSet.addUTXO(tx.outputs[i], tx.txHash, static_cast<int>(i));
    }

    // 8. Supply + base fee adjust
    totalSupply += reward;
    adjustBaseFee(newBlock.gasUsed, /*targetGas*/ 1000000);

    // Info
    std::cout << "Block added. Hash: " << newBlock.hash
              << " | BaseFee: " << newBlock.baseFee
              << " | Nonce: " << newBlock.nonce
              << " | Reward: " << reward << std::endl;
}

// -------------------------------
// Chain validation
// -------------------------------

bool Blockchain::validateBlock(const Block &b, const Block &prev) {
    if (b.previousHash != prev.hash) return false;
    if (b.timestamp <= prev.timestamp) return false;
    if (b.baseFee < 1) return false;
    return true;
}

bool Blockchain::validateChain() {
    for (size_t i = 1; i < chain.size(); ++i)
        if (!validateBlock(chain[i], chain[i - 1]))
            return false;
    return true;
}

// -------------------------------
// Debug Dump
// -------------------------------

void Blockchain::printChain() const {
    std::cout << "------ MedorCoin Chain ------\n";
    for (size_t i = 0; i < chain.size(); ++i) {
        const Block &b = chain[i];
        std::cout << "Block " << i
                  << " | Hash: " << b.hash
                  << " | Prev: " << b.previousHash
                  << " | BaseFee: " << b.baseFee
                  << " | GasUsed: " << b.gasUsed
                  << " | TXCount: " << b.transactions.size()
                  << "\n";
    }
}
