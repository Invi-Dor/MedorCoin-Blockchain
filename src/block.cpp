#include "block.h"

#include <sstream>
#include <ctime>
#include <string>
#include <cstdint>

// Default constructor
Block::Block()
    : previousHash(""),
      data(""),
      difficulty(0),
      minerAddress(""),
      timestamp(static_cast<uint64_t>(time(nullptr))),
      nonce(0),
      reward(0),
      hash("") {}

// Parameterized constructor
Block::Block(const std::string& prevHash,
             const std::string& blockData,
             uint32_t diff,
             const std::string& minerAddr)
    : previousHash(prevHash),
      data(blockData),
      difficulty(diff),
      minerAddress(minerAddr),
      timestamp(static_cast<uint64_t>(time(nullptr))),
      nonce(0),
      reward(0),
      hash("") {}

// Convert block header to string for hashing
std::string Block::headerToString() const {
    std::stringstream ss;
    ss << previousHash
       << timestamp
       << nonce
       << difficulty
       << minerAddress;
    return ss.str();
}
