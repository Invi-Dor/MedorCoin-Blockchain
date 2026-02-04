#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <ctime>
#include "block.h"
#include "crypto.h" // ensure doubleSHA256(string input) exists

using namespace std;

class Blockchain {
public:
    vector<Block> chain;
    uint32_t medor;

    uint64_t totalSupply;
    uint64_t maxSupply;

    string ownerAddress;
    map<string, uint64_t> balances; // tracks coins per address

    // Constructor
    Blockchain(string ownerAddr) {
        ownerAddress = ownerAddr;
        medor = 0x1e00ffff; // difficulty
        totalSupply = 0;
        maxSupply = 50000000;

        // pre-mine genesis block
        chain.push_back(createGenesisBlock());
    }

    // Genesis block
    Block createGenesisBlock() {
        Block genesis("0", "Genesis Block", medor, ownerAddress);
        genesis.timestamp = time(nullptr);

        genesis.reward = 100; // pre-mine 100 MEDOR
        totalSupply += genesis.reward;

        // Split rewards
        balances[ownerAddress] += genesis.reward; // all goes to owner in genesis

        genesis.hash = doubleSHA256(genesis.headerToString());
        return genesis;
    }

    // Dynamic reward
    uint64_t calculateReward() {
        time_t now = time(nullptr);
        time_t genesisTime = chain[0].timestamp;
        double months = difftime(now, genesisTime) / (60*60*24*30.0);

        if (months <= 2.0) return 55;
        else return 30;
    }

    // Lightweight mining (phone-friendly)
    void mineBlock(Block &block) {
        string targetPrefix = "0"; // easy PoW
        do {
            block.nonce++;
            block.hash = doubleSHA256(block.headerToString());
        } while (block.hash.substr(0, targetPrefix.size()) != targetPrefix);
    }

    // Add a new block mined by minerAddress
    void addBlock(const string& data, const string& minerAddr) {
        uint64_t reward = calculateReward();
        if (totalSupply + reward > maxSupply)
            reward = maxSupply - totalSupply;

        Block newBlock(chain.back().hash, data, medor, minerAddr);
        newBlock.timestamp = time(nullptr);
        newBlock.reward = reward;

        mineBlock(newBlock);

        chain.push_back(newBlock);

        // Split reward: 90% miner, 10% owner
        uint64_t minerReward = reward * 0.9;
        uint64_t ownerReward = reward - minerReward;

        balances[minerAddr] += minerReward;
        balances[ownerAddress] += ownerReward;

        totalSupply += reward;
    }

    // Print blockchain and balances
    void printChain() const {
        for (size_t i = 0; i < chain.size(); i++) {
            cout << "Block " << i << ":\n";
            cout << "  Previous Hash: " << chain[i].previousHash << "\n";
            cout << "  Data: " << chain[i].data << "\n";
            cout << "  Hash: " << chain[i].hash << "\n";
            cout << "  Timestamp: " << chain[i].timestamp << "\n";
            cout << "  Reward: " << chain[i].reward << " MEDOR\n";
            cout << "  Miner: " << chain[i].minerAddress << "\n\n";
        }

        cout << "=== Balances ===\n";
        for (auto &entry : balances) {
            cout << entry.first << ": " << entry.second << " MEDOR\n";
        }
        cout << "Total Supply: " << totalSupply << " MEDOR\n\n";
    }
};

/* ---------- MAIN ---------- */
int main() {
    // Owner address
    string owner = "OWNER_ADDRESS_123";

    Blockchain medorCoin(owner);

    // Miners mine blocks
    medorCoin.addBlock("First MedorCoin block", "MINER_A");
    medorCoin.addBlock("Second MedorCoin block", "MINER_B");
    medorCoin.addBlock("Third MedorCoin block", "MINER_A");

    medorCoin.printChain();

    return 0;
}
