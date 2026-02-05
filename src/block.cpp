// In block.h
#pragma once
#include <string>
#include <vector>
#include "transaction.h"

class Block {
public:
    std::string previousHash;
    std::string data;
    uint32_t medor;
    uint64_t nonce;
    uint64_t reward;
    time_t timestamp;
    std::string minerAddress;
    std::vector<Transaction> transactions;

    Block(const std::string& prevHash, const std::string& d, uint32_t diff, const std::string& miner, uint64_t rw = 0)
        : previousHash(prevHash), data(d), medor(diff), nonce(0), reward(rw), timestamp(time(nullptr)), minerAddress(miner) {}
};
