#include "api.h"
#include "auth.h"                        // ➤ ADDED
#include "blockchain.h"
#include "crypto/keystore.h"
#include <nlohmann/json.hpp> // For JSON handling; include your JSON library header

using json = nlohmann::json;

// Starts the server (your HTTP engine code here)
void startAPIServer() {
    // Initialize HTTP server and attach routes

    // ➤ NEW: API key generation route
    app.route("/api/apikey/new", "POST", [](const crow::request& req, crow::response& res) {
        APIKey k = registerNewKey();                         
        res.code = 200;
        res.write(json({{"apiKey", k.key}}).dump());         
        res.end();
    });

    // Transaction routes
    app.route("/api/tx/create", "POST", createTransactionHandler);
    app.route("/api/tx/sign", "POST", signTransactionHandler);
    app.route("/api/tx/broadcast", "POST", broadcastTransactionHandler);
}

// POST /api/tx/create
void createTransactionHandler(const crow::request& req, crow::response& res) {
    if (!checkApiKey(req, res)) return;                     // ➤ ADDED

    auto jsonBody = json::parse(req.body);
    std::string from = jsonBody["from"];
    std::string to = jsonBody["to"];
    uint64_t amount = jsonBody["amount"];
    uint64_t fee = jsonBody["fee"];

    Transaction tx = createTransaction(from, to, amount, fee);
    res.code = 200;
    res.write(tx.toJson()); // Converting transaction to JSON for the response
    res.end();
}

// POST /api/tx/sign
void signTransactionHandler(const crow::request& req, crow::response& res) {
    if (!checkApiKey(req, res)) return;                     // ➤ ADDED

    auto jsonBody = json::parse(req.body);
    Transaction tx = Transaction::fromJson(jsonBody["transaction"]); // Assuming fromJson exists
    std::string privKeyHex = jsonBody["privKeyHex"];

    Transaction signedTx = signTransaction(tx, privKeyHex);
    res.code = 200;
    res.write(signedTx.toJson());
    res.end();
}

// POST /api/tx/broadcast
void broadcastTransactionHandler(const crow::request& req, crow::response& res) {
    if (!checkApiKey(req, res)) return;                     // ➤ ADDED

    auto jsonBody = json::parse(req.body);
    Transaction tx = Transaction::fromJson(jsonBody); // Assuming fromJson exists

    if (broadcastTransaction(tx)) {
        res.code = 200;
        res.write("{\"status\": \"Transaction broadcasted\"}");
    } else {
        res.code = 400;
        res.write("{\"error\": \"Failed to broadcast transaction\"}");
    }
    res.end();
}

// Existing UTXO, transaction, and history handling functions
std::vector<UTXO> getUTXOs(const std::string& address) {
    return blockchain.utxoSet.getUTXOsForAddress(address);
}

Transaction createTransaction(const std::string& from, const std::string& to, uint64_t amount, uint64_t fee) {
    Transaction tx;
    tx.addInputsForAmount(blockchain.utxoSet.getUTXOsForAddress(from), amount + fee);
    tx.addOutput(to, amount);
    tx.addOutput(from, tx.getChange()); // Return any change back to sender
    return tx;
}

Transaction signTransaction(const Transaction& tx, const std::string& privKeyHex) {
    Transaction signedTx = tx;
    for (auto& in : signedTx.inputs) {
        in.signature = Keystore::signInput(in, privKeyHex);
    }
    return signedTx;
}

bool broadcastTransaction(const Transaction& tx) {
    blockchain.addTransactionToMempool(tx);
    return true;
}

std::vector<Transaction> getTransactionHistory(const std::string& address) {
    std::vector<Transaction> history;
    for (const auto& tx : blockchain.chain) {
        if (tx.involvesAddress(address)) {
            history.push_back(tx);
        }
    }
    return history;
}
