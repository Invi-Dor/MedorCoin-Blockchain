#pragma once

#include <string>
#include <vector>
#include <ctime>
#include <cstdint>
#include "transaction.h"

class Block {
public:
    std::string              previousHash;
    std::string              data;
    unsigned int             difficulty  = 0;
    std::string              minerAddress;
    time_t                   timestamp   = 0;
    unsigned long            nonce       = 0;
    uint64_t                 reward      = 0;
    uint64_t                 baseFee     = 0;
    uint64_t                 gasUsed     = 0;
    std::string              hash;
    std::string              signature;
    std::vector<Transaction> transactions;

    Block();
    Block(const std::string& prevHash,
          const std::string& blockData,
          unsigned int       diff,
          const std::string& minerAddr);

    std::string headerToString()              const;
    std::string serialize()                   const;
    void        deserialize(const std::string& data);
    std::vector<uint8_t> serializeHeader()    const;
};
