GET /api/utxos
POST /api/tx/create
POST /api/tx/sign
POST /api/tx/broadcast
GET /api/tx/history

// API.cpp
#include "api.h"
#include "blockchain.h"

// GET /api/tx/history?address={address}
std::vector<Transaction> getTransactionHistory(const std::string& address) {
    std::vector<Transaction> history;
    for (const auto& tx : blockchain.transactions) {
        if (tx.hasAddress(address)) {
            history.push_back(tx);
        }
    }
    return history;
}
