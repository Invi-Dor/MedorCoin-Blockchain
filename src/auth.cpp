#include "auth.h"
#include <unordered_map>
#include <random>

// In‑memory store for API keys — replace with DB if needed
static std::unordered_map<std::string, APIKey> apiKeyStore;

// Generates a 32‑character random key
std::string generateRandomKey() {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    std::string key;
    key.reserve(32);

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, sizeof(alphanum) - 2);

    for (int i = 0; i < 32; i++)
        key += alphanum[dist(rng)];

    return key;
}

// Checks header X-API-Key, enforces 2 free uses before payment
bool checkApiKey(const crow::request& req, crow::response& res) {
    auto it = req.headers.find("X-API-Key");

    if (it == req.headers.end()) {
        res.code = 401;
        res.write("{\"error\":\"API key missing\"}");
        res.end();
        return false;
    }

    std::string key = it->second;
    auto found = apiKeyStore.find(key);

    if (found == apiKeyStore.end()) {
        res.code = 403;
        res.write("{\"error\":\"Invalid API key\"}");
        res.end();
        return false;
    }

    APIKey &entry = found->second;

    // Limit: 2 free uses then block with 429
    if (!entry.paid && entry.usageCount >= 2) {
        res.code = 429;
        res.write("{\"error\":\"Free API limit reached. Upgrade needed.\"}");
        res.end();
        return false;
    }

    entry.usageCount++;
    return true;
}

// Creates a new API key and stores it
APIKey registerNewKey() {
    APIKey k;
    k.key = generateRandomKey();
    apiKeyStore[k.key] = k;
    return k;
}
