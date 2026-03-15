#include "crow.h"
#include "transaction.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <random>
#include <cstdlib>
#include <iostream>
#include "receipts_store.h"
#include <memory>
#include <fstream>
#include <string>
#include <vector>
#include <cctype>

using json = nlohmann::json;

// ------------------------ Global Receipts Store ------------------------
static std::unique_ptr<ReceiptsStore> g_receiptsStore = nullptr;

// ------------------------ Helper: Build Receipt ------------------------
static json buildReceipt(const Transaction &tx,
                         const std::string &productName,
                         const std::string &level,
                         double price,
                         const std::string &buyerName) {
    json receipt;
    receipt["transaction"] = {
        {"txHash", tx.txHash},
        {"buyer", buyerName},
        {"product", productName},
        {"level", level},
        {"price", price},
        {"timestamp", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())}
    };
    return receipt;
}

// ------------------------ Helper: Ingest TX File (NDJSON or JSON array) ------------------------
static void ingestTxFile(const std::string &path) {
    if (path.empty()) return;

    std::ifstream in(path, std::ios::in);
    if (!in.is_open()) {
        std::cerr << "[Receipts] Failed to open TX file: " << path << std::endl;
        return;
    }

    // Detect format: skip leading whitespace and peek first non-space char
    in.seekg(0, std::ios::beg);
    char firstNonWS = '\0';
    int ch;
    while ((ch = in.peek()) != EOF) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            in.get();
            continue;
        } else {
            firstNonWS = static_cast<char>(ch);
            break;
        }
    }

    in.clear();
    in.seekg(0, std::ios::beg);

    size_t lineNo = 0;
    if (firstNonWS == '[') {
        // JSON array form: read entire content
        json arr;
        try {
            std::string content((std::istreambuf_iterator<char>(in)),
                                (std::istreambuf_iterator<char>()));
            if (content.empty()) return;
            arr = json::parse(content);
            if (!arr.is_array()) return;
            for (const auto &item : arr) {
                ++lineNo;
                Transaction tx;
                tx.txHash = item.value("txHash", "");
                std::string buyer  = item.value("buyer", "");
                std::string product = item.value("product", "");
                std::string level  = item.value("level", "");
                double price = item.value("price", 0.0);

                if (tx.txHash.empty()) {
                    tx.txHash = "tx_" + buyer + "_" + product;
                }

                json receipt = buildReceipt(tx, product, level, price, buyer);
                if (g_receiptsStore) {
                    const json& t = receipt["transaction"];
                    std::string txHash = t.value("txHash", "");
                    if (!txHash.empty()) {
                        g_receiptsStore->put(txHash, receipt);
                    }
                }
            }
        } catch (const std::exception &e) {
            std::cerr << "[Receipts] Failed to ingest TX array: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[Receipts] Failed to ingest TX array (unknown error)" << std::endl;
        }
    } else {
        // NDJSON: one JSON object per line
        std::string line;
        while (std::getline(in, line)) {
            ++lineNo;
            if (line.empty()) continue;
            try {
                json item = json::parse(line);

                Transaction tx;
                tx.txHash = item.value("txHash", "");
                std::string buyer  = item.value("buyer", "");
                std::string product = item.value("product", "");
                std::string level  = item.value("level", "");
                double price = item.value("price", 0.0);

                if (tx.txHash.empty()) {
                    tx.txHash = "tx_" + buyer + "_" + product;
                }

                json receipt = buildReceipt(tx, product, level, price, buyer);

                if (g_receiptsStore) {
                    const json& t = receipt["transaction"];
                    std::string txHash = t.value("txHash", "");
                    if (!txHash.empty()) {
                        g_receiptsStore->put(txHash, receipt);
                    }
                }
            } catch (const std::exception &e) {
                std::cerr << "[Receipts] Skipping invalid TX line " << lineNo
                          << " in '" << path << "': " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[Receipts] Skipping invalid TX line " << lineNo
                          << " in '" << path << "'" << std::endl;
            }
        }
    }
}

// ------------------------ Start API Server ------------------------
void startAPIServer() {
    crow::SimpleApp app;

    // Initialize RocksDB-backed storage
    const char* pathEnv = std::getenv("RECEIPTS_ROCKSDB_PATH");
    std::string dbPath = pathEnv ? pathEnv : "./receipts.rocksdb";
    g_receiptsStore = std::make_unique<ReceiptsStore>(dbPath);

    // Ingest TX file at startup (optional)
    const char* txFileEnv = std::getenv("RECEIPTS_TX_FILE");
    if (txFileEnv && txFileEnv[0] != '\0') {
        ingestTxFile(txFileEnv);
    }

    // ------------------------ POST /api/transaction/receipt ------------------------
    CROW_ROUTE(app, "/api/transaction/receipt")
    ([&](const crow::request& req) {
        std::string buyer, product, level, txHash;
        double price = 0.0;

        // Prefer JSON body if provided; otherwise fall back to query params
        if (!req.body.empty()) {
            try {
                json j = json::parse(req.body);
                txHash = j.value("txHash", "");
                buyer = j.value("buyer", "");
                product = j.value("product", "");
                level = j.value("level", "");
                price = j.value("price", 0.0);
            } catch (...) {
                // Fall through to query-param parsing if JSON is invalid
            }
        }

        if (buyer.empty() || product.empty() || level.empty() || price <= 0.0) {
            // Fall back to legacy query params if JSON path failed or not provided
            if (req.has_param("buyer") && req.has_param("product") &&
                req.has_param("level") && req.has_param("price")) {
                buyer = req.get_param_value("buyer");
                product = req.get_param_value("product");
                level = req.get_param_value("level");
                try {
                    price = std::stod(req.get_param_value("price"));
                } catch (...) {
                    price = 0.0;
                }
                txHash = "tx_" + buyer + "_" + product;
            }
        }

        if (txHash.empty() && !buyer.empty() && !product.empty()) {
            txHash = "tx_" + buyer + "_" + product;
        }

        if (buyer.empty() || product.empty() || level.empty() || price <= 0.0) {
            return crow::response(400, "Missing required fields (buyer, product, level, price)");
        }

        Transaction tx;
        tx.txHash = txHash;

        json receipt = buildReceipt(tx, product, level, price, buyer);

        if (g_receiptsStore) {
            std::string h = receipt["transaction"].value("txHash", "");
            if (!h.empty()) g_receiptsStore->put(h, receipt);
        }

        crow::response res;
        res.code = 200;
        res.set_header("Content-Type", "application/json");
        res.write(receipt.dump(4));
        return res;
    });

    // ------------------------ GET /api/receipt/<txHash> ------------------------
    CROW_ROUTE(app, "/api/receipt/<string>")
    ([&](const crow::request& /*req*/, crow::response &res, std::string txHash){
        if (!g_receiptsStore) {
            res.code = 500;
            res.write("{\"error\":\"Receipt store not initialized\"}");
            return;
        }

        json value;
        if (g_receiptsStore->get(txHash, value)) {
            res.code = 200;
            res.set_header("Content-Type", "application/json");
            res.write(value.dump(4));
        } else {
            res.code = 404;
            res.write("{\"error\":\"Receipt not found for txHash\"}");
        }
    });

    // ------------------------ Run server ------------------------
    app.port(18081).multithreaded().run();
}
