#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <string>
#include <vector>
#include <cstdint>

class Transaction {
public:
    uint64_t chainId = 0;
    uint64_t nonce = 0;
    uint64_t maxPriorityFeePerGas = 0;
    uint64_t maxFeePerGas = 0;
    uint64_t gasLimit = 0;
    std::string toAddress;
    uint64_t value = 0;
    std::vector<uint8_t> data;

    // Signature fields
    uint64_t v = 0;
    std::vector<uint8_t> r;
    std::vector<uint8_t> s;

    // The computed transaction hash
    std::string txHash;

    void calculateHash();
};

#endif // TRANSACTION_H
