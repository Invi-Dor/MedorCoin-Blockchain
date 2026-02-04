#include "miner.h"
#include "blockchain.h"

void Miner::mineMedor(Blockchain &chain, const std::string &minerAddress) {
    chain.addBlock("Mined MedorCoin block", minerAddress);
}
