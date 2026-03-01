#include <crow.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <optional>
#include <string>

using json = nlohmann::json;

// Write callback for libcurl
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Per-request fetch from CoinGecko, returning optional JSON
static std::optional<json> fetch_price_json(const std::string& ids, const std::string& vs) {
    // Per-request CURL handle (thread-safe)
    CURL* eh = curl_easy_init();
    if (!eh) {
        std::cerr << "CURL init failed" << std::endl;
        return std::nullopt;
    }

    // Escape parameters
    char* ids_esc = curl_easy_escape(eh, ids.c_str(), (int)ids.length());
    char* vs_esc  = curl_easy_escape(eh, vs.c_str(),  (int)vs.length());
    if (!ids_esc || !vs_esc) {
        if (ids_esc) curl_free(ids_esc);
        if (vs_esc)  curl_free(vs_esc);
        curl_easy_cleanup(eh);
        return std::nullopt;
    }

    std::string buffer;
    std::string url = "https://api.coingecko.com/api/v3/simple/price?ids="
                    + std::string(ids_esc) +
                    "&vs_currencies=" + std::string(vs_esc);

    curl_free(ids_esc);
    curl_free(vs_esc);

    curl_easy_setopt(eh, CURLOPT_URL, url.c_str());
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(eh, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(eh, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(eh, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode res = curl_easy_perform(eh);
    if (res != CURLE_OK) {
        std::cerr << "CURL error: " << curl_easy_strerror(res) << std::endl;
        curl_easy_cleanup(eh);
        return std::nullopt;
    }

    long http_code = 0;
    curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(eh);

    if (http_code != 200) {
        std::cerr << "HTTP error: " << http_code << std::endl;
        return std::nullopt;
    }

    try {
        json j = json::parse(buffer);
        return j;
    } catch (const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

void register_medor_to_usdt_route(crow::SimpleApp &app) {
    // GET endpoint. Optional query params: ids, vs
    // Example: GET /api/convert/medor/usdt?ids=medorcoin&vs=usd
    CROW_ROUTE(app, "/api/convert/medor/usdt").methods("GET"_method)
    ([](const crow::request& req, crow::response& res){
        std::string ids = "medorcoin";
        std::string vs  = "usd";

        // Read query params if provided
        if (auto qids = req.url_params.get("ids"); !qids.empty()) {
            ids = qids;
        }
        if (auto qvs = req.url_params.get("vs"); !qvs.empty()) {
            vs = qvs;
        }

        auto price_json = fetch_price_json(ids, vs);
        if (!price_json.has_value()) {
            res.code = 500;
            res.set_header("Content-Type", "application/json");
            res.write("{\"error\":\"Price unavailable\"}");
            res.end();
            return;
        }

        // Return the full JSON response from CoinGecko
        res.set_header("Content-Type", "application/json");
        res.write(price_json.value().dump());
        res.end();
    });
}

int main() {
    // Initialize libcurl globally (one-time)
    curl_global_init(CURL_GLOBAL_DEFAULT);

    crow::SimpleApp app;
    register_medor_to_usdt_route(app);
    app.port(8080).run();

    curl_global_cleanup();
    return 0;
}

