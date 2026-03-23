#pragma once

#include <string>
#include <cstdint>

class Blockchain;
class Mempool;

class Miner {
public:
    static void mineMedor      (Blockchain        &chain,
                                 const std::string &minerAddress);

    static void mineWithMempool(Blockchain        &chain,
                                 const std::string &minerAddress,
                                 Mempool           &mempool,
                                 uint64_t           baseFee);
};
