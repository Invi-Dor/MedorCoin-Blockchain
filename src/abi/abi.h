#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// A single parameter in ABI
struct ABIParam {
    std::string name;
    std::string type;
};

// ABI function definition
struct ABIFunction {
    std::string name;
    std::string signature;
    std::vector<ABIParam> inputs;
    std::vector<ABIParam> outputs;
};

class ABIParser {
public:
    // Parse the JSON ABI text and return all function entries
    static std::vector<ABIFunction> parseABI(const std::string &jsonText);

    // Generate the 4‑byte selector from a function signature
    static std::vector<uint8_t> generateSelector(const std::string &funcSignature);
};
