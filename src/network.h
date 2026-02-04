#ifndef NETWORK_H
#define NETWORK_H

#include <vector>
#include <string>
#include "transaction.h"
#include "blockchain.h"

class Network {
public:
    std::vector<std::string> peers;

    void connectToPeer(const std::string& address);
    void broadcastTransaction(const Transaction& tx);
    void broadcastBlock(const Block& block);
};

#endif
