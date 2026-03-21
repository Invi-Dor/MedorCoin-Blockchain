#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <queue>
#include <string>
#include <vector>

// =============================================================================
// RECEIPT LOG
// =============================================================================
struct ReceiptLog {
    std::string              address;
    std::vector<std::string> topics;
    std::vector<uint8_t>     data;
};

// =============================================================================
// TRANSACTION RECEIPT
// =============================================================================
struct TransactionReceipt {
    std::string              transactionHash;
    std::string              blockHash;
    uint64_t                 blockNumber       = 0;
    uint64_t                 transactionIndex  = 0;
    std::string              from;
    std::string              to;
    uint64_t                 gasUsed           = 0;
    uint64_t                 cumulativeGasUsed = 0;
    std::vector<ReceiptLog>  logs;
    std::array<uint8_t, 256> logsBloom{};
    bool                     status            = false;
};

// =============================================================================
// BLOOM FILTER HELPER
// =============================================================================
inline void updateBloom(std::array<uint8_t, 256>& bloom,
                         const std::string& value) noexcept
{
    for (int i = 0; i < 3; ++i) {
        size_t pos = 0;
        for (size_t j = 0; j < value.size(); ++j)
            pos = pos * 31 + static_cast<uint8_t>(value[j]) + i;
        pos %= (256 * 8);
        bloom[pos / 8] |= static_cast<uint8_t>(1 << (pos % 8));
    }
}
