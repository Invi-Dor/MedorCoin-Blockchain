#pragma once

#include <string>
#include "blockchain.h"

class Miner {
public:
    // Mine a MedorCoin block
    void mineMedor(Blockchain &chain, const std::string &minerAddress);

    // Optionally include mempool transactions
    void mineWithMempool(Blockchain &chain,
                         const std::string &minerAddress);
};
