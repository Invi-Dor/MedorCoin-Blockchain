#include "block.h"
#include <sstream>
#include <ctime>

Block::Block()
    : previousHash(""),
      data(""),
      difficulty(0),
      minerAddress(""),
      timestamp(time(nullptr)),
      nonce(0),
      reward(0),
      hash("") {}

Block::Block(const std::string& prevHash,
             const std::string& blockData,
             unsigned int diff,
             const std::string& minerAddr)
    : previousHash(prevHash),
      data(blockData),
      difficulty(diff),
      minerAddress(minerAddr),
      timestamp(time(nullptr)),
      nonce(0),
      reward(0),
      hash("") {}

std::string Block::headerToString() const {
    std::stringstream ss;
    ss << previousHash
       << timestamp
       << nonce
       << difficulty
       << minerAddress;
    return ss.str();
}
