#pragma once
#include <string>
#include "blockchain.h"

class Miner {
public:
    void mineMedor(Blockchain &chain, const std::string &minerAddress);
};
