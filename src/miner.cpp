#include "miner.h"
#include "blockchain.h"
#include <string>

// Mine a MedorCoin block
void Miner::mineMedor(Blockchain &chain, const std::string &minerAddress) {
    // The transactions vector could be empty for mining reward only
    std::vector<Transaction> txs;

    // Add block to the chain
    chain.addBlock(minerAddress, txs);
}
