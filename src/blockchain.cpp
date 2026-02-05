#include "blockchain.h"
#include <ctime>
#include <cstdint>
#include <vector>
#include <string>
#include <cstdlib>

// Constructor
Blockchain::Blockchain(const std::string& ownerAddr) {
    ownerAddress = ownerAddr;
    medor = 0x1e00ffff;
    totalSupply = 0;
    maxSupply = 50000000;
}

// Calculate reward
uint64_t Blockchain::calculateReward() {
    time_t now = time(nullptr);
    time_t genesisTime = chain.empty() ? now : chain[0].timestamp;
    double months = difftime(now, genesisTime) / (60 * 60 * 24 * 30.0);
    return months <= 2.0 ? 55 : 30;
}

// Lightweight mining
void Blockchain::mineBlock(Block& block) {
    std::string target = "0";
    do {
        block.nonce++;
        block.hash = "hash" + std::to_string(rand()); // placeholder for doubleSHA256
    } while (block.hash.substr(0, target.size()) != target);
}

// Add block
void Blockchain::addBlock(const std::string& minerAddress, std::vector<Transaction>& transactions) {
    uint64_t reward = calculateReward();
    if (totalSupply + reward > maxSupply)
        reward = maxSupply - totalSupply;

    Transaction coinbaseTx;
    TxOutput minerOut, ownerOut;

    minerOut.value = (reward * 90) / 100;
    minerOut.address = minerAddress;

    ownerOut.value = reward - minerOut.value;
    ownerOut.address = ownerAddress;

    coinbaseTx.outputs.push_back(minerOut);
    coinbaseTx.outputs.push_back(ownerOut);
    coinbaseTx.calculateHash();

    transactions.insert(transactions.begin(), coinbaseTx);

    Block newBlock("", "", medor, minerAddress);
    if (!chain.empty())
        newBlock = Block(chain.back().hash, "MedorCoin Block", medor, minerAddress);

    newBlock.timestamp = time(nullptr);
    newBlock.reward = reward;
    newBlock.transactions = transactions;

    mineBlock(newBlock);
    chain.push_back(newBlock);

    for (auto& tx : newBlock.transactions) {
        for (auto& in : tx.inputs)
            utxoSet.spendUTXO(in.prevTxHash, in.outputIndex);
        for (size_t i = 0; i < tx.outputs.size(); ++i)
            utxoSet.addUTXO(tx.outputs[i], tx.txHash, static_cast<int>(i));
    }

    totalSupply += reward;
}
