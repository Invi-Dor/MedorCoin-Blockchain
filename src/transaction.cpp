#include "transaction.h"
#include "crypto/keccak256.h"
#include <sstream>
#include <iomanip>

bool Transaction::calculateHash() {
    try {
        std::ostringstream ss;
        ss << chainId << nonce << maxPriorityFeePerGas
           << maxFeePerGas << gasLimit << toAddress << value;
        for (const auto& b : data)
            ss << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(b);
        for (const auto& in : inputs)
            ss << in.prevTxHash << in.outputIndex;
        for (const auto& out : outputs)
            ss << out.value << out.address;

        std::string raw = ss.str();
        crypto::Keccak256Digest digest{};
        if (!crypto::Keccak256(
                reinterpret_cast<const uint8_t*>(raw.data()),
                raw.size(), digest))
            return false;

        std::ostringstream hexss;
        for (auto byte : digest)
            hexss << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(byte);
        txHash = hexss.str();
        return true;
    } catch (...) {
        return false;
    }
}

bool Transaction::isValid() const {
    if (txHash.empty())  return false;
    if (txHash.size() != 64) return false;
    return true;
}
