#pragma once

#include <array>
#include <cstdint>
#include <vector>

void updateBloom(std::array<uint8_t,256> &bloom, const std::vector<uint8_t> &data);
void updateBloom(std::array<uint8_t,256> &bloom, const std::array<uint8_t,20> &addr);
void updateBloom(std::array<uint8_t,256> &bloom, const std::array<uint8_t,32> &topic);
