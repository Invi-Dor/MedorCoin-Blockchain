#pragma once
#include <crow.h>
#include <string>

// Check API key validity & usage limit
bool checkApiKey(const crow::request& req, crow::response& res);

// Generate a random 32-character API key
std::string generateRandomKey();
