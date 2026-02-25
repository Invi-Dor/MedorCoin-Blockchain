#pragma once

#include <string>
#include <vector>
#include <array>
#include <unordered_map>

class ValidatorRegistry {
public:
    // Load the validator list (hard-coded or config)
    static void loadValidators();

    // Check if an address (20‑byte) is a validator
    static bool isValidator(const std::array<uint8_t,20> &addr);

    // Lookup validator private key (hex) from known map
    static std::string getPrivateKey(const std::string &addrHex);

    static const std::vector<std::array<uint8_t,20>>& getValidators();

private:
    static std::vector<std::array<uintuint8_t,20>> validators;
};
