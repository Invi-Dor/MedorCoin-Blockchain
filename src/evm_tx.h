#pragma once
#include <vector>
#include <array>
#include <cstdint>

struct EvmTx {
    uint64_t nonce;
    uint64_t gasPrice;
    uint64_t gasLimit;
    std::vector<uint8_t> to;      // 20 bytes
    uint64_t value;               // amount in base units
    std::vector<uint8_t> data;    // contract input or empty
    std::array<uint8_t,32> r;     // signature r
    std::array<uint8_t,32> s;     // signature s
    uint8_t v;                    // recovery id
};
