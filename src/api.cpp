#include "crow.h"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <ctime>                 // for timestamps
#include "blockchain.h"          // for on-chain verification registry access
// ... other includes as before ...

using json = nlohmann::json;

// Simple helper to fetch a URL (libcurl)
static std::string fetchUrl(const std::string& url) {
    CURL* curl = curl_easy_init();
    if(!curl) return "";
    std::string buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append(ptr, size*nmemb); return size*nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return buffer;
}

// GET /api/swap/quote?chainId=1&fromTokenAddress=0x...&toTokenAddress=0x...&amount=1000000000000000000
void swapQuoteHandler(const crow::request& req, crow::response& res) {
    auto chainIdStr = req.url_params.get("chainId");
    auto fromToken = req.url_params.get("fromTokenAddress");
    auto toToken = req.url_params.get("toTokenAddress");
    auto amount = req.url_params.get("amount");

    if (!chainIdStr || !fromToken || !toToken || !amount) {
        res.code = 400;
        res.write("{\"error\":\"Missing required params\"}");
        return;
    }

    // 1inch quote URL (v5)
    std::string url = "https://api.1inch.io/v5.0/" + std::string(chainIdStr) +
                      "/quote?fromTokenAddress=" + fromToken +
                      "&toTokenAddress=" + toToken +
                      "&amount=" + amount;
    std::string body = fetchUrl(url);
    res.code = 200;
    res.set_header("Content-Type", "application/json");
    res.write(body);
    res.end();
}

// GET /api/swap/execute?chainId=1&fromTokenAddress=0x...&toTokenAddress=0x...&amount=...&fromAddress=0x...&slippage=0.5
void swapExecuteHandler(const crow::request& req, crow::response& res) {
    auto chainIdStr = req.url_params.get("chainId");
    auto fromToken = req.url_params.get("fromTokenAddress");
    auto toToken = req.url_params.get("toTokenAddress");
    auto amount = req.url_params.get("amount");
    auto fromAddress = req.url_params.get("fromAddress");
    auto slippage = req.url_params.get("slippage");
    if (!chainIdStr || !fromToken || !toToken || !amount || !fromAddress) {
        res.code = 400;
        res.write("{\"error\":\"Missing required params\"}");
        return;
    }

    // 1inch swap endpoint (returns a tx object to be signed by wallet)
    std::string url = "https://api.1inch.io/v5.0/" + std::string(chainIdStr) +
                      "/swap?fromTokenAddress=" + fromToken +
                      "&toTokenAddress=" + toToken +
                      "&amount=" + amount +
                      "&fromAddress=" + fromAddress +
                      "&slippage=" + (slippage ? slippage : "0.5");
    std::string body = fetchUrl(url);
    res.code = 200;
    res.set_header("Content-Type", "application/json");
    res.write(body);
    res.end();
}

// NEW: Verifications API integration (production-ready path)

// 1) MedorCoin address constant (replace with your real address when deploying)
static std::string MEDOR_COIN_ADDRESS = "0xMEDOR_COIN_ADDRESS_REPLACE"; // <-- Replace with your actual MedorCoin address

// 2) Verification endpoints
// POST /api/verify/address
void verifyAddressHandler(const crow::request& req, crow::response& res) {
    // API key protection (reuse existing pattern)
    if (!checkApiKey(req, res)) return;

    auto body = json::parse(req.body);
    std::string address = body.value("address", "");
    std::string metadata = body.value("metadata", "");
    std::string verifier = body.value("verifier", "");

    if (address.empty() || metadata.empty() || verifier.empty()) {
        res.code = 400;
        res.write("{\"error\":\"Missing parameters (address, metadata, verifier)\"}");
        res.end();
        return;
    }

    // Persist on-chain registry (no change to token transfer logic)
    bool ok = false;
    try {
        // If you implemented the registry API in Blockchain, this should work.
        ok = blockchain.verifyAddress(address, metadata, verifier);
    } catch (...) {
        ok = false;
    }

    json resp;
    resp["address"] = address;
    resp["verified"] = ok;
    resp["verifier"] = verifier;
    resp["verifiedAt"] = static_cast<uint64_t>(time(nullptr));

    res.code = ok ? 200 : 500;
    res.set_header("Content-Type", "application/json");
    res.write(resp.dump());
    res.end();

    // Optional: broadcast verification to peers (not shown here; wire into your NetworkManager if desired)
}

// GET /api/address_status?address=<addr>
void addressStatusHandler(const crow::request& req, crow::response& res) {
    auto addrParam = req.url_params.get("address");
    if (!addrParam) {
        res.code = 400;
        res.write("{\"error\":\"Missing address parameter\"}");
        res.end();
        return;
    }

    // Query the on-chain registry (assumes Blockchain exposes these helpers)
    bool verified = false;
    std::string address = *addrParam;
    try {
        verified = blockchain.isAddressVerified(address);
    } catch (...) {
        verified = false;
    }

    json out;
    out["address"] = address;
    out["verified"] = verified;

    res.code = 200;
    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}

// 3) Wire new routes into the public API server
void startApiPublicServer() {
    // Initialize/attach routes to the existing Crow app (same style as your production server)
    // Ensure you register after you construct the app object (not shown here in this snippet)

    // NEW: verification endpoints
    app.route("/api/verify/address", "POST", verifyAddressHandler);
    app.route("/api/address_status", "GET", addressStatusHandler);

    // Existing routes remain unchanged...
    // e.g., /api/apikey/new, /api/tx/create, etc.
    // Other existing routes continue to be registered below as in your original file.
}
