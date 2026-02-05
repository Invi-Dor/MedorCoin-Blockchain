#include "blockchain.h"
#include "block.h"
#include "transaction.h"
#include "utxo.h"
#include "consensus.h"

#include <ctime>      // for time()
#include <cstdint>    // for uint32_t, uint64_t
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h> // OpenSSL SHA256

// Helper: compute SHA256 as hex string
static std::string sha256(const std::string &data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256Ctx;
    SHA256_Init(&sha256Ctx);
    SHA256_Update(&sha256Ctx, data.c_str(), data.size());
    SHA256_Final(hash, &sha256Ctx);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

// Bitcoin‑style double SHA256: SHA256(SHA256(input))
static std::string doubleSHA256(const std::string &input) {
    return sha256(sha256(input));
}

// Constructor
Blockchain::Blockchain(const std::string& ownerAddr) {
    ownerAddress = ownerAddr;
    medor = 0x1e00ffff;
    totalSupply = 0;
    maxSupply = 50000000;
    chain.clear();
    utxoSet.utxos.clear();
}

// Calculate reward
uint64_t Blockchain::calculateReward() {
    time_t now = time(nullptr);
    time_t genesisTime = chain.empty() ? now : chain[0].timestamp;
    double months = difftime(now, genesisTime) / (60.0 * 60.0 * 24.0 * 30.0);
    return (months <= 2.0) ? 55 : 30;
}

// Lightweight mining
void Blockchain::mineBlock(Block& block) {
    std::string targetPrefix = "0";
    do {
        block.nonce++;
        block.hash = doubleSHA256(block.headerToString());
    } while (block.hash.substr(0, targetPrefix.size()) != targetPrefix);
}

// Add block
void Blockchain::addBlock(const std::string& minerAddress,
                          std::vector<Transaction>& transactions) {

    uint64_t reward = calculateReward();
    if (totalSupply + reward > maxSupply) {
        reward = maxSupply - totalSupply;
    }

    // Create coinbase transaction
    Transaction coinbaseTx;
    TxOutput minerOut;
    TxOutput ownerOut;

    minerOut.value = (reward * 90) / 100;
    minerOut.address = minerAddress;

    ownerOut.value = reward - minerOut.value;
    ownerOut.address = ownerAddress;

    coinbaseTx.outputs.push_back(minerOut);
    coinbaseTx.outputs.push_back(ownerOut);
    coinbaseTx.calculateHash();

    // Insert coinbase at start
    transactions.insert(transactions.begin(), coinbaseTx);

    // Create new block
    Block newBlock;
    if (chain.empty()) {
        newBlock = Block("", "MedorCoin Block", medor, minerAddress);
    } else {
        newBlock = Block(chain.back().hash, "MedorCoin Block", medor, minerAddress);
    }

    newBlock.timestamp = time(nullptr);
    newBlock.reward = reward;
    newBlock.transactions = transactions;

    // Mine it
    mineBlock(newBlock);

    // Add to chain
    chain.push_back(newBlock);

    // Update UTXO set
    for (auto& tx : newBlock.transactions) {
        for (auto& in : tx.inputs) {
            utxoSet.spendUTXO(in.prevTxHash, in.outputIndex);
        }
        for (size_t i = 0; i < tx.outputs.size(); ++i) {
            utxoSet.addUTXO(tx.outputs[i], tx.txHash, static_cast<int>(i));
        }
    }

    // Increase supply
    totalSupply += reward;
}
