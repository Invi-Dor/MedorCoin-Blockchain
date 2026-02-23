#include "auth.h"
#include <unordered_map>
#include <random>

// Structure to hold each API key and its usage
struct APIKey {
    std::string key;
    int usageCount = 0;
    bool paid = false;
};

// Store all keys in memory (replace with DB later if needed)
static std::unordered_map<std::string, APIKey> apiKeyStore;

// Generates a random 32-character key
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

// Middleware to check API key validity & enforce free key limits
bool checkApiKey(const crow::request& req, crow::response& res) {
    auto it = req.headers.find("X-API-Key");
    if (it == req.headers.end()) {
        res.code = 401;
        res.write("{\"error\":\"API key missing\"}");
        res.end();
        return false;
    }

    std::string key = it->second;
    if (!apiKeyStore.count(key)) {
        res.code = 403;
        res.write("{\"error\":\"Invalid API key\"}");
        res.end();
        return false;
    }

    auto &k = apiKeyStore[key];

    // Free limit: only 2 uses allowed for free keys
    if (!k.paid && k.usageCount >= 2) {
        res.code = 429;
        res.write("{\"error\":\"Free API limit reached. Upgrade for more.\"}");
        res.end();
        return false;
    }

    k.usageCount++; // Increment usage
    return true;
}

// Optional helper to register a new key (used by /api/apikey/new)
APIKey registerNewKey() {
    std::string key = generateRandomKey();
    APIKey k;
    k.key = key;
    apiKeyStore[key] = k;
    return k;
}
