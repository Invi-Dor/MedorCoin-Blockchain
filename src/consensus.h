#ifndef CONSENSUS_H
#define CONSENSUS_H

#include "blockchain.h"
#include "block.h"

class Consensus {
public:
    static bool validateBlock(const Block& block, const Block& previousBlock);
    static bool validateChain(const Blockchain& chain);
};

#endif
