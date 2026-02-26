#pragma once

#include <cstdint>

class FeeHelper {
public:
    static uint64_t recommendedBaseFee(uint64_t lastBaseFee);
    static uint64_t recommendedPriority(uint64_t recentTipAvg);
};
