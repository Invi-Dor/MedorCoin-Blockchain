#include "blockchain.h"
#include "miner.h"
#include <iostream>

int main() {
    std::string owner = "OWNER_ADDR_ABC";
    Blockchain medorChain(owner);

    Miner miner;

    miner.mineMedor(medorChain, "MINER_1");
    miner.mineMedor(medorChain, "MINER_2");
    miner.mineMedor(medorChain, "MINER_1");

    medorChain.printChain();

    return 0;
}
