#include <crow.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static json getCoinGeckoPrice(const std::string& ids, const std::string& vs) {
    CURL* curl = curl_easy_init();
    std::string buffer;
    std::string url = "https://api.coingecko.com/api/v3/simple/price?ids=" + ids + "&vs_currencies=" + vs;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return json::parse(buffer);
}

void register_medor_to_usdt_route(crow::SimpleApp &app) {
    CROW_ROUTE(app, "/api/convert/medor/usdt").methods("POST"_method)
    ([](const crow::request&, crow::response& res){
        json price = getCoinGeckoPrice("medorcoin", "usdt");
        if (!price.contains("medorcoin")) {
            res.code = 500;
            res.write("{\"error\":\"Price unavailable\"}");
        } else {
            res.set_header("Content-Type", "application/json");
            res.write(price.dump());
        }
        res.end();
    });
}
