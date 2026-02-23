#pragma once

#include <string>
#include <crow.h>

// This holds each API key with usage count and paid status
struct APIKey {
    std::string key;
    int usageCount = 0;
    bool paid = false;
};

// Called inside your handlers — returns false and ends response if not valid
bool checkApiKey(const crow::request& req, crow::response& res);

// Generates a new random API key string
std::string generateRandomKey();

// Registers and stores a new key — returns a record you can send to clients
APIKey registerNewKey();
