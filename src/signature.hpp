#pragma once
#include <vector>
#include <array>

std::tuple<std::array<uint8_t,32>, std::array<uint8_t,32>, uint8_t>
signHash(const std::array<uint8_t,32> &digest,
         const std::string &privKeyPath);
