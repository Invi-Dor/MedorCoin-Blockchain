#pragma once

#include <string>
#include <atomic>

// Represents a stored API key entry
struct APIKey {
    std::string key;          // The API key string
    bool paid;                // Whether the key is paid (true) or free (false)
    std::atomic<uint64_t> usageCount{0}; // Usage counter (thread‑safe)
};

// Register a new paid API key
APIKey registerNewPaidKey();

// Register a new free API key (if needed)
APIKey registerNewFreeKey();

// Look up an API key in the store
APIKey* findApiKey(const std::string &key);

// Generate a secure random API key string
std::string generateRandomKey();
