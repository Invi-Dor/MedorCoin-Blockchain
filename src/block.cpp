#include <iostream>
#include <vector>
#include "block.h"

using namespace std;

class Blockchain {
public:
    vector<Block> chain;
    uint32_t medor;

    Blockchain() {
        medor = 0x1e00ffff; // Bitcoin-equivalent difficulty
        chain.push_back(createGenesisBlock());
    }

    Block createGenesisBlock() {
        return Block("0", "Genesis Block", medor);
    }

    void addBlock(string data) {
        chain.push_back(Block(chain.back().hash, data, medor));
    }
};

/* ---------- MAIN ---------- */
int main() {
    Blockchain medorCoin;

    medorCoin.addBlock("First MedorCoin block");
    medorCoin.addBlock("Second MedorCoin block");

    return 0;
}
