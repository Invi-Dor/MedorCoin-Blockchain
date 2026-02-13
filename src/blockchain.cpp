#include "blockchain.h"
#include "crypto/verify_signature.h"
#include <sstream>
#include <iomanip>

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

    if (!blockDB.open("data/medorcoin_blocks"))
        std::cerr << "[BlockDB] Failed to open blocks DB" << std::endl;

    leveldb::Iterator *it = blockDB.db->NewIterator(leveldb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        Block b;
        std::string key = it->key().ToString();
        if (blockDB.readBlock(key, b)) {
            chain.push_back(b);
        }
    }
    delete it;

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

uint64_t Blockchain::getBalance(const std::array<uint8_t,20> &addr) const {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (auto b : addr) ss << std::setw(2) << (int)b;
    return getBalance(ss.str());
}

void Blockchain::setBalance(const std::array<uint8_t,20> &addr, uint64_t amount) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (auto b : addr) ss << std::setw(2) << (int)b;
    setBalance(ss.str(), amount);
}

void Blockchain::addBalance(const std::array<uint8_t,20> &addr, uint64_t amount) {
    uint64_t current = getBalance(addr);
    setBalance(addr, current + amount);
}

// -------------------------------
// Base Fee
// -------------------------------

uint64_t Blockchain::getCurrentBaseFee() const {
    return baseFeePerGas;
}

void Blockchain::setCurrentBaseFee(uint64_t fee) {
    baseFeePerGas = (fee < 1 ? 1 : fee);
}

void Blockchain::burnBaseFees(uint64_t amount) {
    const std::string treasuryAddr = "medor_treasury";
    addBalance(treasuryAddr, amount);
}

void Blockchain::adjustBaseFee(uint64_t gasUsed, uint64_t gasLimit) {
    if (gasLimit == 0) return;
    if (gasUsed * 2 > gasLimit) {
        baseFeePerGas += baseFeePerGas / 8;
    } else {
        baseFeePerGas = (baseFeePerGas > 1 ? baseFeePerGas - baseFeePerGas / 8 : 1);
    }
}

// -------------------------------
// Verify Signature
// -------------------------------

bool Blockchain::verifyTransactionSignature(const Transaction &tx) const {
    std::array<uint8_t,32> hashBytes;
    for (size_t i = 0; i < 32; i++) {
        std::string byteHex = tx.txHash.substr(i * 2, 2);
        hashBytes[i] = static_cast<uint8_t>(std::stoul(byteHex, nullptr, 16));
    }
    return verifyEvmSignature(
        hashBytes,
        tx.r, tx.s, tx.v,
        tx.fromAddress
    );
}

// -------------------------------
// Execute Transaction
// -------------------------------

bool Blockchain::executeTransaction(const Transaction &tx,
                                    const std::string &minerAddress)
{
    uint64_t senderBal = getBalance(tx.fromAddress);
    if (senderBal < tx.value) return false;
    setBalance(tx.fromAddress, senderBal - tx.value);
    addBalance(tx.toAddress, tx.value);
    return true;
}

// -------------------------------
// Fetch Transactions by Hashes
// -------------------------------

std::vector<Transaction>
Blockchain::getTransactions(const std::vector<std::string> &hashes) const
{
    std::vector<Transaction> out;
    for (auto &h : hashes) {
        for (auto &blk : chain) {
            for (auto &tx : blk.transactions) {
                if (tx.txHash == h) {
                    out.push_back(tx);
                    break;
                }
            }
        }
    }
    return out;
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
// Add Block
// -------------------------------

void Blockchain::addBlock(const std::string &minerAddr,
                          std::vector<Transaction> &transactions)
{
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

    Block newBlock = chain.empty()
        ? Block("", "Genesis Block", medor, minerAddr)
        : Block(chain.back().hash, "MedorCoin Block", medor, minerAddr);

    newBlock.timestamp = time(nullptr);
    newBlock.reward = reward;
    newBlock.transactions = transactions;
    newBlock.baseFee = baseFeePerGas;

    mineBlock(newBlock);

    if (!ProofOfWork(medor).validateBlock(newBlock)) {
        return;
    }

    chain.push_back(newBlock);
    blockDB.writeBlock(newBlock);

    for (auto &tx : newBlock.transactions) {
        for (auto &in : tx.inputs)
            utxoSet.spendUTXO(in.prevTxHash, in.outputIndex);
        for (size_t i = 0; i < tx.outputs.size(); ++i)
            utxoSet.addUTXO(tx.outputs[i], tx.txHash, static_cast<int>(i));
    }

    totalSupply += reward;
    adjustBaseFee(newBlock.gasUsed, 1000000);
}

// -------------------------------
// Chain Validation / Debug
// -------------------------------

bool Blockchain::validateBlock(const Block &block,
                              const Block &prevBlock)
{
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

void Blockchain::printChain() const {
    for (size_t i = 0; i < chain.size(); ++i) {
        const Block &b = chain[i];
        std::cout << "Block " << i
                  << " | Hash: " << b.hash
                  << " | BaseFee: " << b.baseFee
                  << " | TXCount: " << b.transactions.size()
                  << std::endl;
    }
}
