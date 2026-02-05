#pragma once

#include <string>
#include <vector>
#include <ctime>

struct Transaction; // forward declaration if needed

class Block {
public:
    std::string previousHash;
    std::string data;
    unsigned int difficulty;
    std::string minerAddress;
    time_t timestamp;       // correct C++ type from <ctime>
    unsigned long nonce;
    unsigned long reward;
    std::string hash;
    std::vector<Transaction> transactions;

    Block();
    Block(const std::string& prevHash,
          const std::string& blockData,
          unsigned int diff,
          const std::string& minerAddr);

    std::string headerToString() const;
};
