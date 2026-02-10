#pragma once
#include <vector>
#include <string>

// Helpers to encode ABI types
class ABIEncoder {
public:
    static std::vector<uint8_t> encodeAddress(const std::string &addr);
    static std::vector<uint8_t> encodeUint256(const std::string &value);
    static std::vector<uint8_t> encodeBool(bool v);

    // Build full calldata: selector + encoded args
    static std::vector<uint8_t> encodeCall(
        const std::vector<uint8_t> &selector,
        const std::vector<std::vector<uint8_t>> &args);
};
