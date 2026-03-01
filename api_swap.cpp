 #include "crow.h"
#include <nlohmann/json.hpp>
#include <curl/curl.h>

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

void startApiSwapServer() {
    Crow<DemoApp> app;
    // NOTE: Your actual Crow app instance type may differ (keep consistent with your codebase)
    app.route("/api/swap/quote", "GET", swapQuoteHandler);
    app.route("/api/swap/execute", "GET", swapExecuteHandler);
    // If you already have an app, register routes onto it rather than creating a new one.
    app.port(8080).run();
}

