#pragma once

#include <string>
#include <vector>
#include "transaction.h"
#include "block.h"

class Network {
public:
    std::vector<std::string> peers;

    void connectToPeer(const std::string& address);
    void broadcastTransaction(const Transaction& tx);
    void broadcastBlock(const Block& block);
};
