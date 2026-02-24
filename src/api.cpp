#include "api.h"
#include "auth.h"                        // ➤ ADDED
#include "blockchain.h"
#include "crypto/keystore.h"
#include <nlohmann/json.hpp> // For JSON handling; include your JSON library header

using json = nlohmann::json;

// Forward declarations for new handlers
void getLatestTransactionsHandler(const crow::request& req, crow::response& res);
void getLatestBlocksHandler(const crow::request& req, crow::response& res);
void getNewTokensHandler(const crow::request& req, crow::response& res);
void searchHandler(const crow::request& req, crow::response& res);
void getTransactionHandler(const crow::request& req, crow::response& res, std::string txId);
void getBlockHandler(const crow::request& req, crow::response& res, int blockIndex);
void getAddressHandler(const crow::request& req, crow::response& res, std::string address);

// Starts the server (your HTTP engine code here)
void startAPIServer() {
    // Initialize HTTP server and attach routes

    // ➤ API key generation
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

    // <<<<< NEW EXPLORER / FRONTEND ROUTES >>>>>
    app.route("/api/txs", "GET", getLatestTransactionsHandler);
    app.route("/api/blocks", "GET", getLatestBlocksHandler);
    app.route("/api/tokens/new", "GET", getNewTokensHandler);
    app.route("/api/search", "GET", searchHandler);
    app.route_dynamic("/api/tx/<string>")(getTransactionHandler);
    app.route_dynamic("/api/block/<int>")(getBlockHandler);
    app.route_dynamic("/api/address/<string>")(getAddressHandler);
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

// -------------------
// NEW FRONTEND / EXPLORER HANDLERS
// -------------------

// GET latest 10 transactions
void getLatestTransactionsHandler(const crow::request& req, crow::response& res) {
    std::vector<Transaction> latest = blockchain.getLatestTransactions(10);
    json arr = json::array();
    for (auto& tx : latest) arr.push_back(json::parse(tx.toJson()));
    res.code = 200;
    res.write(arr.dump());
    res.end();
}

// GET latest 10 blocks
void getLatestBlocksHandler(const crow::request& req, crow::response& res) {
    std::vector<Block> latest = blockchain.getLatestBlocks(10);
    json arr = json::array();
    for (auto& b : latest) arr.push_back(json::parse(b.toJson()));
    res.code = 200;
    res.write(arr.dump());
    res.end();
}

// GET newly created tokens
void getNewTokensHandler(const crow::request& req, crow::response& res) {
    auto tokens = blockchain.getNewTokens(10);
    json arr = json::array();
    for (auto& t : tokens) arr.push_back(json::parse(t.toJson()));
    res.code = 200;
    res.write(arr.dump());
    res.end();
}

// GET search
void searchHandler(const crow::request& req, crow::response& res) {
    auto query = req.url_params.get("q");
    if (!query) {
        res.code = 400;
        res.write("{\"error\":\"Missing query parameter q\"}");
        res.end();
        return;
    }
    auto results = blockchain.searchTokens(query);
    json arr = json::array();
    for (auto& t : results) arr.push_back(json::parse(t.toJson()));
    res.code = 200;
    res.write(arr.dump());
    res.end();
}

// GET transaction by ID
void getTransactionHandler(const crow::request& req, crow::response& res, std::string txId) {
    auto tx = blockchain.getTransaction(txId);
    if (!tx) {
        res.code = 404;
        res.write("{\"error\":\"Transaction not found\"}");
    } else {
        res.code = 200;
        res.write(tx->toJson());
    }
    res.end();
}

// GET block by index
void getBlockHandler(const crow::request& req, crow::response& res, int blockIndex) {
    auto block = blockchain.getBlock(blockIndex);
    if (!block) {
        res.code = 404;
        res.write("{\"error\":\"Block not found\"}");
    } else {
        res.code = 200;
        res.write(block->toJson());
    }
    res.end();
}

// GET address transaction history
void getAddressHandler(const crow::request& req, crow::response& res, std::string address) {
    auto history = getTransactionHistory(address);
    json arr = json::array();
    for (auto& tx : history) arr.push_back(json::parse(tx.toJson()));
    res.code = 200;
    res.write(arr.dump());
    res.end();
}

// -------------------
// Existing UTXO, transaction, and history handling functions
// -------------------
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
