#include "consensus/validator_registry.h"

// Hard‑coded validator addresses (in hex, 40 characters)
static const std::vector<std::string> INITIAL_VALIDATORS = {
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    // add more addresses here as needed
};

std::vector<std::array<uint8_t,20>> ValidatorRegistry::validators;

static std::array<uint8_t,20> hexToAddress(const std::string &hex) {
    std::array<uint8_t,20> addr{};
    for (size_t i = 0; i < 20; ++i) {
        std::string byteHex = hex.substr(i * 2, 2);
        addr[i] = static_cast<uint8_t>(std::stoul(byteHex, nullptr, 16));
    }
    return addr;
}

void ValidatorRegistry::loadValidators() {
    validators.clear();
    for (auto &h : INITIAL_VALIDATORS) {
        validators.push_back(hexToAddress(h));
    }
}

bool ValidatorRegistry::isValidator(const std::array<uint8_t,20> &addr) {
    for (auto &v : validators) {
        if (v == addr) return true;
    }
    return false;
}

const std::vector<std::array<uint8_t,20>>& ValidatorRegistry::getValidators() {
    return validators;
}
