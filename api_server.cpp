#include "crow.h"
#include "utxo.h"
#include "transaction.h"
#include "blockchain.h"
#include "net/serialization.h"
#include "feehelper.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// =============================================================================
// EXTERNAL DEPENDENCIES
// These must be provided by main() or node startup and passed in via
// setApiServerContext() before app.run() is called.
// =============================================================================
static UTXOSet   *g_utxoSet          = nullptr;
static Blockchain *g_blockchain       = nullptr;
static uint64_t   g_latestBaseFee    = 1;
static uint64_t   g_recentTipAvg     = 1;

void setApiServerContext(UTXOSet   *utxoSet,
                         Blockchain *blockchain,
                         uint64_t    latestBaseFee,
                         uint64_t    recentTipAvg)
{
    g_utxoSet       = utxoSet;
    g_blockchain    = blockchain;
    g_latestBaseFee = latestBaseFee;
    g_recentTipAvg  = recentTipAvg;
}

// =============================================================================
// INTERNAL HELPER
// Extracts txHash from a raw hex transaction by deserializing it.
// Returns empty string on failure.
// =============================================================================
static std::string extractTxHash(const std::string &rawHex)
{
    try {
        auto j = json::parse(rawHex);
        Transaction tx = deserializeTx(j);
        return tx.txHash;
    } catch (...) {
        return "";
    }
}

// =============================================================================
// GET /api/balance?address=<address>
// =============================================================================
void balanceHandler(const crow::request &req, crow::response &res)
{
    if (!g_utxoSet) {
        res.code = 503;
        res.write("{\"error\":\"service not ready\"}");
        res.end();
        return;
    }
    auto addrPtr = req.url_params.get("address");
    if (!addrPtr) {
        res.code = 400;
        res.write("{\"error\":\"missing address\"}");
        res.end();
        return;
    }
    std::string address(addrPtr);
    // getBalance returns std::optional<uint64_t> -- use value_or(0)
    uint64_t balance = g_utxoSet->getBalance(address).value_or(0);

    json out;
    out["address"] = address;
    out["balance"] = balance;
    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

// =============================================================================
// GET /api/utxos?address=<address>
// =============================================================================
void utxosHandler(const crow::request &req, crow::response &res)
{
    if (!g_utxoSet) {
        res.code = 503;
        res.write("{\"error\":\"service not ready\"}");
        res.end();
        return;
    }
    auto addrPtr = req.url_params.get("address");
    if (!addrPtr) {
        res.code = 400;
        res.write("{\"error\":\"missing address\"}");
        res.end();
        return;
    }
    std::string address(addrPtr);

    json arr = json::array();
    // getUTXOsForAddress is the correct method -- allFor() does not exist
    for (const auto &utxo : g_utxoSet->getUTXOsForAddress(address)) {
        json u;
        u["txid"]  = utxo.txHash;
        u["vout"]  = utxo.outputIndex;  // outputIndex not index
        u["value"] = utxo.value;
        arr.push_back(u);
    }

    json out;
    out["address"] = address;
    out["utxos"]   = arr;
    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

// =============================================================================
// GET /api/txs?address=<address>
// Transaction history is derived from UTXOs for this address.
// A full TxStore is not part of the current architecture -- this endpoint
// returns the current unspent outputs for the address as a proxy.
// =============================================================================
void txHistoryHandler(const crow::request &req, crow::response &res)
{
    if (!g_utxoSet) {
        res.code = 503;
        res.write("{\"error\":\"service not ready\"}");
        res.end();
        return;
    }
    auto addrPtr = req.url_params.get("address");
    if (!addrPtr) {
        res.code = 400;
        res.write("{\"error\":\"missing address\"}");
        res.end();
        return;
    }
    std::string address(addrPtr);

    json arr = json::array();
    for (const auto &utxo : g_utxoSet->getUTXOsForAddress(address)) {
        json t;
        t["txHash"]      = utxo.txHash;
        t["outputIndex"] = utxo.outputIndex;
        t["value"]       = utxo.value;
        t["blockHeight"] = utxo.blockHeight;
        t["isCoinbase"]  = utxo.isCoinbase;
        arr.push_back(t);
    }

    json out;
    out["address"]  = address;
    out["utxos"]    = arr;
    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

// =============================================================================
// GET /api/fee
// =============================================================================
void feeHandler(const crow::request & /*req*/, crow::response &res)
{
    uint64_t base = FeeHelper::recommendedBaseFee(g_latestBaseFee);
    uint64_t tip  = FeeHelper::recommendedPriority(g_recentTipAvg);

    json out;
    out["baseFee"]     = base;
    out["priorityFee"] = tip;
    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

// =============================================================================
// POST /api/broadcast
// Body: { "rawTxHex": "<json-serialized transaction>" }
// =============================================================================
void broadcastHandler(const crow::request &req, crow::response &res)
{
    if (!g_blockchain) {
        res.code = 503;
        res.write("{\"error\":\"service not ready\"}");
        res.end();
        return;
    }

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

    Transaction tx;
    try {
        tx = deserializeTx(json::parse(rawHex));
    } catch (const std::exception &e) {
        res.code = 400;
        json out;
        out["error"] = std::string("transaction deserialization failed: ") + e.what();
        res.write(out.dump());
        res.end();
        return;
    }

    // addBlock with a single transaction -- miner address is the tx recipient
    std::string minerAddr = tx.outputs.empty() ? "" : tx.outputs.front().address;
    bool ok = g_blockchain->addBlock(minerAddr, { tx });

    json out;
    if (!ok) {
        out["error"] = "broadcast failed";
        res.code = 500;
    } else {
        out["txid"] = tx.txHash;
        res.code = 200;
    }
    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

int main()
{
    crow::SimpleApp app;

    CROW_ROUTE(app, "/api/balance").methods("GET"_method)(balanceHandler);
    CROW_ROUTE(app, "/api/utxos").methods("GET"_method)(utxosHandler);
    CROW_ROUTE(app, "/api/txs").methods("GET"_method)(txHistoryHandler);
    CROW_ROUTE(app, "/api/fee").methods("GET"_method)(feeHandler);
    CROW_ROUTE(app, "/api/broadcast").methods("POST"_method)(broadcastHandler);

    app.port(8080).multithreaded().run();
    return 0;
}
