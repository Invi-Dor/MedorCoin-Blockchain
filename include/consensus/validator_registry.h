#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class ValidatorRegistry {
public:
    static void loadValidators();
    static bool isValidator(const std::array<uint8_t, 20>& addr);
    static std::string getPrivateKey(const std::string& addrHex);
    static const std::vector<std::array<uint8_t, 20>>& getValidators();

private:
    static std::vector<std::array<uint8_t, 20>> validators;
};
