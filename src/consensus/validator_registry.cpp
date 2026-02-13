#include "consensus/validator_registry.h"

// Hard‑coded validator list — replace with config or on‑chain later
static const std::vector<std::string> VALIDATOR_HEX = {
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
};

std::vector<std::array<uint8_t,20>> ValidatorRegistry::validators;

// Simple mapping of address → private key (hex)
// For real net this *must* be secured (HSM, KMS, encrypted config).
static const std::unordered_map<std::string, std::string> KEYMAP = {
    {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","<privatekey_hex_1>"},
    {"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","<privatekey_hex_2>"}
};

static std::array<uint8_t,20> hexToAddr(const std::string &hex) {
    std::array<uint8_t,20> addr{};
    for (size_t i = 0; i < 20; ++i) {
        addr[i] = static_cast<uint8_t>(std::stoul(hex.substr(i*2,2), nullptr, 16));
    }
    return addr;
}

void ValidatorRegistry::loadValidators() {
    validators.clear();
    for (const auto &h : VALIDATOR_HEX) {
        validators.push_back(hexToAddr(h));
    }
}

bool ValidatorRegistry::isValidator(const std::array<uint8_t,20> &addr) {
    for (auto &v : validators) {
        if (v == addr) return true;
    }
    return false;
}

std::string ValidatorRegistry::getPrivateKey(const std::string &addrHex) {
    auto it = KEYMAP.find(addrHex);
    if (it != KEYMAP.end()) return it->second;
    return "";
}

const std::vector<std::array<uint8_t,20>>& ValidatorRegistry::getValidators() {
    return validators;
}
