#include "merkle.h"
#include "crypto.h"
#include <iostream>

using namespace std;

MerkleTree::MerkleTree(const vector<Transaction>& transactions) {
    vector<string> txHashes;
    for (auto &tx : transactions) {
        txHashes.push_back(tx.txid);
    }
    root = computeMerkleRoot(txHashes);
}

string MerkleTree::computeMerkleRoot(vector<string> hashes) {
    if (hashes.empty()) return "";

    while (hashes.size() > 1) {
        vector<string> newLevel;

        for (size_t i = 0; i < hashes.size(); i += 2) {
            string left = hashes[i];
            string right = (i + 1 < hashes.size()) ? hashes[i + 1] : left;
            newLevel.push_back(doubleSHA256(left + right));
        }

        hashes = newLevel;
    }

    return hashes[0];
}
