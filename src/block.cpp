#include "block.h"
#include "crypto/keccak256.h"
#include <sstream>
#include <iomanip>

Block::Block()
    : previousHash(""), data(""), difficulty(0),
      minerAddress(""), timestamp(time(nullptr)),
      nonce(0), reward(0), baseFee(0), gasUsed(0),
      hash(""), signature("") {}

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
      baseFee(0),
      gasUsed(0),
      hash(""),
      signature("") {}

std::string Block::headerToString() const {
    std::ostringstream ss;
    ss << previousHash << timestamp << nonce
       << difficulty << minerAddress
       << baseFee << gasUsed;
    return ss.str();
}

std::vector<uint8_t> Block::serializeHeader() const {
    std::string h = headerToString();
    return std::vector<uint8_t>(h.begin(), h.end());
}

std::string Block::serialize() const {
    std::ostringstream ss;
    ss << previousHash << "|"
       << data         << "|"
       << difficulty   << "|"
       << minerAddress << "|"
       << timestamp    << "|"
       << nonce        << "|"
       << reward       << "|"
       << baseFee      << "|"
       << gasUsed      << "|"
       << hash         << "|"
       << signature    << "|"
       << transactions.size();
    for (const auto& tx : transactions)
        ss << "|" << tx.txHash
           << "|" << tx.toAddress
           << "|" << tx.value
           << "|" << tx.nonce;
    return ss.str();
}

void Block::deserialize(const std::string& raw) {
    std::istringstream ss(raw);
    std::string token;
    auto next = [&]() -> std::string {
        std::getline(ss, token, '|');
        return token;
    };
    previousHash = next();
    data         = next();
    difficulty   = static_cast<unsigned int>(std::stoul(next()));
    minerAddress = next();
    timestamp    = static_cast<time_t>(std::stoll(next()));
    nonce        = std::stoul(next());
    reward       = std::stoull(next());
    baseFee      = std::stoull(next());
    gasUsed      = std::stoull(next());
    hash         = next();
    signature    = next();
    size_t txCount = std::stoull(next());
    transactions.clear();
    for (size_t i = 0; i < txCount; i++) {
        Transaction tx;
        tx.txHash    = next();
        tx.toAddress = next();
        tx.value     = std::stoull(next());
        tx.nonce     = std::stoull(next());
        transactions.push_back(tx);
    }
}
