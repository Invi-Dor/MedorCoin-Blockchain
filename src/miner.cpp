#include <iostream>
#include <vector>

#include "blockchain.h"
#include "transaction.h"
#include "miner.h"

int main() {
    // Create a MedorCoin blockchain with an owner address
    Blockchain medorChain("OWNER_ADDRESS");

    // Example transactions list
    std::vector<Transaction> txs;

    // Example: mine a block
    Miner miner;
    miner.mineMedor(medorChain, "MINER_ADDRESS");

    std::cout << "Block mined successfully!" << std::endl;
    return 0;
}
