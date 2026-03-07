#include "transaction.h"
#include "crypto/rlp.h"
#include "crypto/keccak/keccak.h"

#include <sstream>
#include <iomanip>

static std::string toHex(const std::vector<uint8_t> &bytes) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (auto b : bytes) {
        ss << std::setw(2) << (int)b;
    }
    return ss.str();
}

void Transaction::calculateHash() {
    using namespace rlp;

    // Encode transaction fields in the correct order
    auto encChainId  = encodeUInt(chainId);
    auto encNonce    = encodeUInt(nonce);
    auto encMaxPri   = encodeUInt(maxPriorityFeePerGas);
    auto encMaxFee   = encodeUInt(maxFeePerGas);
    auto encGasLimit = encodeUInt(gasLimit);

    // Encode "to" address as bytes
    std::vector<uint8_t> toBytes(toAddress.begin(), toAddress.end());
    auto encTo       = encodeBytes(toBytes);

    auto encValue    = encodeUInt(value);
    auto encData     = encodeBytes(data);

    // Encode the signature components
    auto encV        = encodeUInt(v);
    auto encR        = encodeBytes(std::vector<uint8_t>(r.begin(), r.end()));
    auto encS        = encodeBytes(std::vector<uint8_t>(s.begin(), s.end()));

    // Put all encoded fields into a list
    std::vector<std::vector<uint8_t>> items = {
        encChainId,
        encNonce,
        encMaxPri,
        encMaxFee,
        encGasLimit,
        encTo,
        encValue,
        encData,
        encV,
        encR,
        encS
    };

    std::vector<uint8_t> raw = encodeList(items);

    // Compute Keccak‑256 hash (transaction hash)
    uint8_t hashOut[32];
    keccak(raw.data(), raw.size(), hashOut, 32);
    std::vector<uint8_t> hashBytes(hashOut, hashOut + 32);

    // Store as hex string
    txHash = toHex(hashBytes);
}
