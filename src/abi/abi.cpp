#include "abi/abi.h"
#include "crypto/keccak/keccak.h"
#include <iostream>
#include <sstream>

// Parse the Solidity JSON ABI and extract function definitions
std::vector<ABIFunction> ABIParser::parseABI(const std::string &jsonText) {
    std::vector<ABIFunction> functions;

    // Parse JSON
    json j = json::parse(jsonText);

    for (const auto &item : j) {
        // Only handle normal functions (skip constructor/events)
        if (!item.contains("type") || item["type"] != "function")
            continue;

        ABIFunction fn;
        fn.name = item.value("name", "");

        // Parse inputs
        if (item.contains("inputs")) {
            for (const auto &inp : item["inputs"]) {
                fn.inputs.push_back({
                    inp.value("name", ""),
                    inp.value("type", "")
                });
            }
        }

        // Parse outputs
        if (item.contains("outputs")) {
            for (const auto &out : item["outputs"]) {
                fn.outputs.push_back({
                    out.value("name", ""),
                    out.value("type", "")
                });
            }
        }

        // Build the canonical function signature, e.g., "transfer(address,uint256)"
        std::ostringstream sig;
        sig << fn.name << "(";
        for (size_t i = 0; i < fn.inputs.size(); ++i) {
            sig << fn.inputs[i].type;
            if (i + 1 < fn.inputs.size())
                sig << ",";
        }
        sig << ")";
        fn.signature = sig.str();

        // Add to the list
        functions.push_back(fn);
    }

    return functions;
}

// Generate the 4‑byte Ethereum function selector using Keccak‑256
std::vector<uint8_t> ABIParser::generateSelector(const std::string &funcSignature) {
    // Keccak‑256 produces 32 bytes
    uint8_t hash[32];
    keccak((const uint8_t*)funcSignature.data(), funcSignature.size(), hash, 32);

    // The selector is the first 4 bytes of the hash
    return std::vector<uint8_t>(hash, hash + 4);
}
