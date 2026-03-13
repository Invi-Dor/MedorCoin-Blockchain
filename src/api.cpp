#include "crow.h"
#include "transaction.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <random>
#include <cstdlib>
#include <iostream>

using json = nlohmann::json;

// Secure API key from environment
static const std::string API_KEY = []() {
    const char* key = std::getenv("MEDOR_API_KEY");
    if (!key || std::string(key).empty()) {
        std::cerr << "[FATAL] MEDOR_API_KEY not set. Exiting." << std::endl;
        std::exit(1);
    }
    return std::string(key);
}();

// Thread-safe random nonce
static uint64_t generateNonce() {
    thread_local static std::mt19937_64 eng(
        static_cast<unsigned long long>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    return eng();
}

// Get UTC timestamp in ISO8601
static std::string utc_now_iso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm g = *std::gmtime(&t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g);
    return std::string(buf);
}

// JSON error response
static void json_error(crow::response &res, int code, const std::string &msg) {
    res.code = code;
    res.set_header("Content-Type", "application/json");
    res.write(json({{"error", msg}}).dump());
    res.end();
}

// Check API key header
static bool checkApiKey(const crow::request &req, crow::response &res) {
    auto key = req.get_header_value("X-API-Key");
    if (key.empty() || key != API_KEY) {
        json_error(res, 401, "Unauthorized: Invalid API key");
        return false;
    }
    return true;
}

// Build a receipt from a transaction
static json buildReceipt(const Transaction &tx,
                         const std::string &productName,
                         const std::string &level,
                         double price,
                         const std::string &buyerName) {
    uint64_t gasUsed = (tx.gasLimit > 0) ? tx.gasLimit : 21000;

    json receipt;
    receipt["transaction"] = {
        {"txHash", tx.txHash},
        {"from", tx.toAddress}, // swap to match buyer/seller logic if needed
        {"to", tx.toAddress},
        {"value", tx.value},
        {"gasUsed", gasUsed},
        {"timestamp", utc_now_iso8601()}
    };
    receipt["purchase"] = {
        {"productName", productName},
        {"level", level},
        {"price", price},
        {"buyerName", buyerName}
    };
    return receipt;
}

void startAPIServer() {
    crow::SimpleApp app;

    // GET /api/transaction/receipt?buyer=...&product=...&level=...&price=...
    CROW_ROUTE(app, "/api/transaction/receipt")
    ([&](const crow::request &req, crow::response &res){
        if (!checkApiKey(req, res)) return;

        auto buyer   = req.url_params.get("buyer");
        auto product = req.url_params.get("product");
        auto level   = req.url_params.get("level");
        auto priceStr = req.url_params.get("price");

        if (!buyer || !product || !level || !priceStr) {
            json_error(res, 400, "Missing required parameters");
            return;
        }

        double price = 0.0;
        try {
            price = std::stod(priceStr);
            if (price < 0) throw std::invalid_argument("negative price");
        } catch (...) {
            json_error(res, 400, "Invalid price parameter");
            return;
        }

        // Create a real Transaction object
        Transaction tx;
        tx.fromAddress = "0xSellerAddress"; // replace with seller or dynamic value
        tx.toAddress   = buyer;
        tx.value       = static_cast<uint64_t>(price * 1e18); // assume wei units
        tx.nonce       = generateNonce();
        tx.gasLimit    = 21000;
        tx.calculateHash(); // fill txHash

        // Build receipt
        json receipt = buildReceipt(tx, product, level, price, buyer);

        res.code = 200;
        res.set_header("Content-Type", "application/json");
        res.write(receipt.dump(4));
        res.end();
    });

    app.port(18080).multithreaded().run();
}
