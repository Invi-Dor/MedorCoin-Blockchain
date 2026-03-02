#include "crow.h"
#include "utxo.h"
#include "transaction.h"
#include "feehelper.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Global UTXO set (your existing backend logic)
static UTXOSet utxoSet;

// -------------------------
// Balance Endpoint
// GET /api/balance?address=<address>
// -------------------------
void balanceHandler(const crow::request &req, crow::response &res) {
    auto addrPtr = req.url_params.get("address");
    if (!addrPtr) {
        res.code = 400;
        res.write("{\"error\":\"missing address\"}");
        res.end();
        return;
    }
    std::string address(addrPtr);
    uint64_t balance = utxoSet.getBalance(address);

    json out;
    out["address"] = address;
    out["balance"] = balance; // in satoshis
    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

// -------------------------
// UTXOs Endpoint
// GET /api/utxos?address=<address>
// -------------------------
void utxosHandler(const crow::request &req, crow::response &res) {
    auto addrPtr = req.url_params.get("address");
    if (!addrPtr) {
        res.code = 400;
        res.write("{\"error\":\"missing address\"}");
        res.end();
        return;
    }
    std::string address(addrPtr);

    json arr = json::array();
    for (const auto &utxo : utxoSet.allFor(address)) {
        json u;
        u["txid"]  = utxo.txHash;
        u["vout"]  = utxo.index;
        u["value"] = utxo.value; // satoshis
        arr.push_back(u);
    }

    json out;
    out["address"] = address;
    out["utxos"]   = arr;

    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

// -------------------------
// Transaction History
// GET /api/txs?address=<address>
// -------------------------
void txHistoryHandler(const crow::request &req, crow::response &res) {
    auto addrPtr = req.url_params.get("address");
    if (!addrPtr) {
        res.code = 400;
        res.write("{\"error\":\"missing address\"}");
        res.end();
        return;
    }
    std::string address(addrPtr);

    json arr = json::array();
    for (const auto &tx : txStore.getHistory(address)) {
        json t;
        t["hash"] = tx.txHash;
        t["inputs"] = tx.inputs;
        t["outputs"] = tx.outputs;
        t["timestamp"] = tx.timestamp;
        arr.push_back(t);
    }

    json out;
    out["address"] = address;
    out["transactions"] = arr;

    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

// -------------------------
// Fee Estimate
// GET /api/fee
// -------------------------
void feeHandler(const crow::request & /*req*/, crow::response &res) {
    // Use your FeeHelper logic
    uint64_t base = FeeHelper::recommendedBaseFee(latestChainBaseFee);
    uint64_t tip  = FeeHelper::recommendedPriority(recentTipAvg);

    json out;
    out["baseFee"] = base;
    out["priorityFee"] = tip;

    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

// -------------------------
// Broadcast Signed TX
// POST /api/broadcast
/*
{
  "rawTxHex": "0200000001..."
}
*/
// -------------------------
void broadcastHandler(const crow::request &req, crow::response &res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (...) {
        res.code = 400;
        res.write("{\"error\":\"invalid JSON\"}");
        res.end();
        return;
    }

    if (!body.contains("rawTxHex")) {
        res.code = 400;
        res.write("{\"error\":\"missing rawTxHex\"}");
        res.end();
        return;
    }

    std::string rawHex = body["rawTxHex"];
    bool ok = blockchain.broadcast(rawHex);

    json out;
    if (!ok) {
        out["error"] = "broadcast failed";
        res.code = 500;
    } else {
        out["txid"] = extractTxHash(rawHex);
        res.code = 200;
    }

    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

int main() {
    crow::SimpleApp app;

    CROW_ROUTE(app, "/api/balance").methods("GET"_method)(balanceHandler);
    CROW_ROUTE(app, "/api/utxos").methods("GET"_method)(utxosHandler);
    CROW_ROUTE(app, "/api/txs").methods("GET"_method)(txHistoryHandler);
    CROW_ROUTE(app, "/api/fee").methods("GET"_method)(feeHandler);
    CROW_ROUTE(app, "/api/broadcast").methods("POST"_method)(broadcastHandler);

    app.port(8080).multithreaded().run();
}
