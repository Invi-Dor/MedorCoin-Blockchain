#pragma once
#include <string>

class Block {
public:
    std::string previousHash;
    std::string data;
    std::string hash;
    uint32_t medor;        // difficulty
    time_t timestamp;
    uint64_t reward;       // reward for this block
    uint64_t nonce;        // lightweight proof of work
    std::string minerAddress; // who mined this block

    Block() : medor(0), reward(0), nonce(0), timestamp(0), minerAddress("") {}

    Block(std::string prev, std::string d, uint32_t diff, std::string miner="")
        : previousHash(prev), data(d), medor(diff),
          reward(0), nonce(0), timestamp(0), minerAddress(miner) {}

    std::string headerToString() const {
        return previousHash + data
            + std::to_string(timestamp)
            + std::to_string(reward)
            + std::to_string(nonce)
            + minerAddress;
    }
};
