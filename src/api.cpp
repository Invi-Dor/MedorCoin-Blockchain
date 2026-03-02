#include "crow.h"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <ctime>
#include <string>

using json = nlohmann::json;

// === Configuration ===

// Replace this with a secure configuration mechanism in production
static const std::string API_KEY = "REPLACE_WITH_SECURE_API_KEY";

// Allowed external API hosts for SSRF protection
static bool isAllowedUrl(const std::string &url) {
    const std::string httpsPrefix = "https://";
    if (url.rfind(httpsPrefix, 0) != 0) return false;

    size_t hostStart = httpsPrefix.size();
    size_t pathStart = url.find('/', hostStart);
    std::string host = (pathStart == std::string::npos)
                           ? url.substr(hostStart)
                           : url.substr(hostStart, pathStart - hostStart);

    // Allowed upstream hosts
    return (host == "api.1inch.io");
}

// === Networking Helpers ===

static size_t writeCallback(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string fetchUrl(const std::string &url) {
    if (!isAllowedUrl(url)) return "";

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return "";
    return buffer;
}

// === API Key Protection ===

static bool checkApiKey(const crow::request &req, crow::response &res) {
    const std::string providedKey = req.get_header_value("X-API-Key");
    if (providedKey.empty() || providedKey != API_KEY) {
        res.code = 401;
        json err;
        err["error"] = "Unauthorized";
        err["message"] = "Invalid or missing API key";
        res.set_header("Content-Type", "application/json");
        res.write(err.dump());
        res.end();
        return false;
    }
    return true;
}

// === Handlers ===

// GET /api/swap/quote
void swapQuoteHandler(const crow::request &req, crow::response &res) {
    auto chainIdStr = req.url_params.get("chainId");
    auto fromToken = req.url_params.get("fromTokenAddress");
    auto toToken = req.url_params.get("toTokenAddress");
    auto amount = req.url_params.get("amount");

    if (!chainIdStr || !fromToken || !toToken || !amount) {
        res.code = 400;
        res.write("{\"error\":\"Missing required parameters\"}");
        res.end();
        return;
    }

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

// GET /api/swap/execute
void swapExecuteHandler(const crow::request &req, crow::response &res) {
    auto chainId = req.url_params.get("chainId");
    auto fromToken = req.url_params.get("fromTokenAddress");
    auto toToken = req.url_params.get("toTokenAddress");
    auto amount = req.url_params.get("amount");
    auto fromAddress = req.url_params.get("fromAddress");
    auto slippage = req.url_params.get("slippage");

    if (!chainId || !fromToken || !toToken || !amount || !fromAddress) {
        res.code = 400;
        res.write("{\"error\":\"Missing required parameters\"}");
        res.end();
        return;
    }

    std::string url = "https://api.1inch.io/v5.0/" + std::string(chainId) +
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

// POST /api/verify/address
void verifyAddressHandler(const crow::request &req, crow::response &res) {
    if (!checkApiKey(req, res)) return;

    json parsed;
    try {
        parsed = json::parse(req.body);
    } catch (const nlohmann::json::parse_error &) {
        res.code = 400;
        res.set_header("Content-Type", "application/json");
        res.write("{\"error\":\"Invalid JSON in request body\"}");
        res.end();
        return;
    }

    std::string address = parsed.value("address", "");
    std::string metadata = parsed.value("metadata", "");
    std::string verifier = parsed.value("verifier", "");

    if (address.empty() || metadata.empty() || verifier.empty()) {
        res.code = 400;
        res.write("{\"error\":\"Missing parameters (address, metadata, verifier)\"}");
        res.end();
        return;
    }

    bool verified = false;
    try {
        verified = blockchain.verifyAddress(address, metadata, verifier);
    } catch (...) {
        verified = false;
    }

    json resp;
    resp["address"] = address;
    resp["verified"] = verified;
    resp["verifier"] = verifier;
    resp["verifiedAt"] = static_cast<uint64_t>(time(nullptr));

    res.code = verified ? 200 : 500;
    res.set_header("Content-Type", "application/json");
    res.write(resp.dump());
    res.end();
}

// GET /api/address_status
void addressStatusHandler(const crow::request &req, crow::response &res) {
    auto addrParam = req.url_params.get("address");
    if (!addrParam) {
        res.code = 400;
        res.write("{\"error\":\"Missing address parameter\"}");
        res.end();
        return;
    }

    bool verified = false;
    try {
        verified = blockchain.isAddressVerified(*addrParam);
    } catch (...) {
        verified = false;
    }

    json out;
    out["address"] = *addrParam;
    out["verified"] = verified;

    res.code = 200;
    res.set_header("Content-Type", "application/json");
    res.write(out.dump());
    res.end();
}
