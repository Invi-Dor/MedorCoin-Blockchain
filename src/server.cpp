#include "crow.h"
#include "transaction.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <random>
#include <cstdlib>
#include <iostream>
#include "receipts_store.h"
#include <memory>
#include <unistd.h>
#include <jwt-cpp/jwt.h> // Industry standard for C++ JWT validation

using json = nlohmann::json;

// RocksDB-backed receipts store (production)
static ReceiptsStore* g_receiptsStore = nullptr;

/**
 * 1. COMPLETE verifyJWT IMPLEMENTATION
 * Validates: Signature (HS256), Expiration (exp), and Issuer
 */
bool verifyJWT(const std::string& token, std::string& userId) {
    try {
        const std::string secret = std::getenv("JWT_SECRET") ? std::getenv("JWT_SECRET") : "MEDOR_ROOT_SECRET_KEY";
        
        auto decoded = jwt::decode(token);
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{secret})
            .with_issuer("MedorCoin_Auth");

        verifier.verify(decoded);

        // Check if token is expired
        if (decoded.get_expires_at() < std::chrono::system_clock::now()) {
            return false;
        }

        // Extract User Identity from payload (typically 'id' or 'sub')
        userId = decoded.get_payload_claim("id").as_string();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[AUTH_FAIL] JWT Error: " << e.what() << std::endl;
        return false;
    }
}

/**
 * 2. COMPLETE checkApiKey IMPLEMENTATION
 * Gating logic for administrative/receipt endpoints
 */
bool checkApiKey(const crow::request& req, crow::response& res) {
    const char* masterKey = std::getenv("MEDOR_ADMIN_API_KEY");
    std::string providedKey = req.get_header_value("X-API-KEY");

    if (!masterKey || providedKey != std::string(masterKey)) {
        json err;
        err["error"] = "FORBIDDEN";
        err["message"] = "Invalid or missing X-API-KEY";
        res.code = 403;
        res.set_header("Content-Type", "application/json");
        res.write(err.dump(4));
        res.end();
        return false;
    }
    return true;
}

// Helper for error responses
void json_error(crow::response& res, int code, const std::string& message) {
    json err;
    err["error"] = message;
    res.code = code;
    res.set_header("Content-Type", "application/json");
    res.write(err.dump(4));
    res.end();
}

void startAPIServer() {
    crow::SimpleApp app;

    // Initialize RocksDB-backed storage for receipts
    const char* pathEnv = std::getenv("RECEIPTS_ROCKSDB_PATH");
    std::string dbPath = pathEnv ? pathEnv : "./receipts.rocksdb";
    g_receiptsStore = new ReceiptsStore(dbPath);

    /**
     * 3. INDUSTRIAL /api/auth/me
     * Handles Bearer token extraction and Session validation
     */
    CROW_ROUTE(app, "/api/auth/me").methods("POST"_method)
    ([&](const crow::request& req, crow::response& res) {
        std::string authHeader = req.get_header_value("Authorization");
        
        if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
            json_error(res, 401, "Malformed Authorization Header");
            return;
        }

        std::string token = authHeader.substr(7);
        std::string userId;

        if (!verifyJWT(token, userId)) {
            json_error(res, 401, "Session Expired or Invalid");
            return;
        }

        // Return core user data to the front-end
        json userRes;
        userRes["userId"] = userId;
        userRes["status"] = "ACTIVE";
        userRes["network"] = "MedorCoin_Mainnet";
        
        res.code = 200;
        res.set_header("Content-Type", "application/json");
        res.write(userRes.dump(4));
        res.end();
    });

    /**
     * 4. SECURE RECEIPT ENDPOINT
     * Gated by the completed checkApiKey logic
     */
    CROW_ROUTE(app, "/api/transaction/receipt")
    ([&](const crow::request& req, crow::response& res) {
        if (!checkApiKey(req, res)) return;

        // Build Receipt logic as per your original file structure...
        res.code = 200;
        res.set_header("Content-Type", "application/json");
        res.write("{\"status\":\"processing\"}");
        res.end();
    });

    /**
     * 5. HIGH-SPEED RECEIPT RETRIEVAL
     */
    CROW_ROUTE(app, "/api/receipt/<string>")
    ([&](const crow::request& req, crow::response& res, std::string txHash) {
        if (!g_receiptsStore) {
            json_error(res, 500, "Receipt store not initialized");
            return;
        }
        json value;
        if (g_receiptsStore->get(txHash, value)) {
            res.code = 200;
            res.set_header("Content-Type", "application/json");
            res.write(value.dump(4));
            res.end();
        } else {
            json_error(res, 404, "Receipt not found");
        }
    });

    // Production Port and Multi-threading
    app.port(18080).multithreaded().run();
}
