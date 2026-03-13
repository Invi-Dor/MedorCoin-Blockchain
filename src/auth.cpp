#include "auth.h"
#include <random>
#include <mutex>
#include <unordered_map>

// Thread‑safe key storage
static std::unordered_map<std::string, APIKey> apiKeyStore;
static std::mutex apiKeyStoreMutex;

// Generate a secure 32‑character random API key
std::string generateRandomKey() {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, sizeof(alphanum) - 2);

    std::string key;
    key.reserve(32);
    for (int i = 0; i < 32; i++) {
        key += alphanum[dist(rng)];
    }
    return key;
}

// Find an API key in the store (returns pointer or nullptr)
APIKey* findApiKey(const std::string &key) {
    std::lock_guard<std::mutex> lock(apiKeyStoreMutex);
    auto it = apiKeyStore.find(key);
    if (it == apiKeyStore.end()) return nullptr;
    return &it->second;
}

// Register a new **paid** key
APIKey registerNewPaidKey() {
    std::lock_guard<std::mutex> lock(apiKeyStoreMutex);
    APIKey k;
    k.key = generateRandomKey();
    k.paid = true;      // Paid key
    k.usageCount = 0;
    apiKeyStore[k.key] = k;
    return apiKeyStore[k.key];
}

// Register a new **free** key
APIKey registerNewFreeKey() {
    std::lock_guard<std::mutex> lock(apiKeyStoreMutex);
    APIKey k;
    k.key = generateRandomKey();
    k.paid = false;     // Free key (limited usage)
    k.usageCount = 0;
    apiKeyStore[k.key] = k;
    return apiKeyStore[k.key];
}
