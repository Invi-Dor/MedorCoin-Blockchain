#include "blockchain.h"
#include "consensus/validator_registry.h"
#include "crypto/poa_sign.h"

#include <ctime>
#include <iostream>

Blockchain::Blockchain(const std::string &ownerAddr)
    : blockDB(), accountDB("data/medorcoin_accounts")
{
    ownerAddress = ownerAddr;
    medor = 0;    
    totalSupply = 0;
    maxSupply = 50000000;

    ValidatorRegistry::loadValidators();

    leveldb::Iterator *it = blockDB.db->NewIterator(leveldb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        Block b;
        if (blockDB.readBlock(it->key().ToString(), b))
            chain.push_back(b);
    }
    delete it;

    if (chain.empty()) {
        std::vector<Transaction> noTxs;
        addBlock(ownerAddress, noTxs);
    }
}

bool Blockchain::isValidator(const std::string &addrHex) const {
    std::array<uint8_t,20> a{};
    for (size_t i = 0; i < 20 && i*2+1 < addrHex.size(); ++i)
        a[i] = static_cast<uint8_t>(std::stoul(addrHex.substr(i*2,2), nullptr, 16));
    return ValidatorRegistry::isValidator(a);
}

void Blockchain::addBlock(const std::string &minerAddress,
                          std::vector<Transaction> &transactions)
{
    if (!isValidator(minerAddress)) {
        std::cerr << "[PoA] Not a validator: " << minerAddress << std::endl;
        return;
    }

    uint64_t reward = 0; // optional for PoA
    Transaction coinbaseTx;
    coinbaseTx.outputs.push_back({reward, minerAddress});
    coinbaseTx.calculateHash();
    transactions.insert(transactions.begin(), coinbaseTx);

    Block newBlock;
    if (!chain.empty()) newBlock = Block(chain.back().hash, "PoA Block", 0, minerAddress);
    else newBlock = Block("", "Genesis Block", 0, minerAddress);

    newBlock.timestamp = time(nullptr);
    newBlock.transactions = transactions;
    newBlock.baseFee = baseFeePerGas;

    newBlock.hash = newBlock.headerToString();

    newBlock.signature = signBlockPoA(newBlock, minerAddress);

    chain.push_back(newBlock);
    blockDB.writeBlock(newBlock);

    for (auto &tx : newBlock.transactions) {
        for (auto &in : tx.inputs)
            utxoSet.spendUTXO(in.prevTxHash, in.outputIndex);
        for (size_t i = 0; i < tx.outputs.size(); ++i)
            utxoSet.addUTXO(tx.outputs[i], tx.txHash, static_cast<int>(i));
    }
}

bool Blockchain::validateBlock(const Block &block, const Block &prevBlock) {
    if (prevBlock.hash != block.previousHash) return false;

    if (!verifyBlockPoA(block)) {
        std::cerr << "[PoA] Bad block signature" << std::endl;
        return false;
    }

    return true;
}

bool Blockchain::validateChain() {
    for (size_t i = 1; i < chain.size(); ++i)
        if (!validateBlock(chain[i], chain[i-1]))
            return false;
    return true;
}
