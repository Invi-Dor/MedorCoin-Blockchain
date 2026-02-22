#include "api.h"
#include "blockchain.h"
#include "crypto/keystore.h"

// Starts the server (your HTTP engine code here)
void startAPIServer() {
    // Initialize HTTP server and attach routes
    // Your server library code (cpp-httplib, crow, civetweb, etc.)
}

// GET /api/utxos?address={address}
std::vector<UTXO> getUTXOs(const std::string& address) {
    return blockchain.utxoSet.getUTXOsForAddress(address);
}

// POST /api/tx/create
Transaction createTransaction(const std::string& from, const std::string& to, uint64_t amount, uint64_t fee) {
    Transaction tx;
    tx.addInputsForAmount(blockchain.utxoSet.getUTXOsForAddress(from), amount + fee);
    tx.addOutput(to, amount);
    tx.addOutput(from, tx.getChange()); // change back
    return tx;
}

// POST /api/tx/sign
Transaction signTransaction(const Transaction& tx, const std::string& privKeyHex) {
    Transaction signedTx = tx;
    for (auto& in : signedTx.inputs) {
        in.signature = Keystore::signInput(in, privKeyHex);
    }
    return signedTx;
}

// POST /api/tx/broadcast
bool broadcastTransaction(const Transaction& tx) {
    blockchain.addTransactionToMempool(tx);
    return true;
}

// GET /api/tx/history?address={address}
std::vector<Transaction> getTransactionHistory(const std::string& address) {
    std::vector<Transaction> history;
    for (const auto& tx : blockchain.chain) {
        if (tx.involvesAddress(address)) {
            history.push_back(tx);
        }
    }
    return history;
}
