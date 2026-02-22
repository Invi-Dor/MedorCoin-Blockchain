#ifndef MEDOR_API_H
#define MEDOR_API_H

#include <string>
#include <vector>
#include "utxo.h"
#include "transaction.h"

// Starts the HTTP API server
void startAPIServer();

// UTXO endpoints
std::vector<UTXO> getUTXOs(const std::string& address);

// Transaction endpoints
Transaction createTransaction(const std::string& from, const std::string& to, uint64_t amount, uint64_t fee);
Transaction signTransaction(const Transaction& tx, const std::string& privKeyHex);
bool broadcastTransaction(const Transaction& tx);

// History
std::vector<Transaction> getTransactionHistory(const std::string& address);

#endif // MEDOR_API_H
