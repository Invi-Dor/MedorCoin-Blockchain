#include "merkle.h"
#include "crypto.h"
#include "transaction.h"
#include <vector>
#include <string>

MerkleTree::MerkleTree(const std::vector<Transaction>& transactions) {
    std::vector<std::string> txHashes;

    // Collect transaction hashes from the transaction vector
    for (const auto& tx : transactions) {
        txHashes.push_back(tx.txHash); // Use the correct Transaction field
    }

    root = computeMerkleRoot(txHashes);
}

std::string MerkleTree::computeMerkleRoot(std::vector<std::string> hashes) {
    if (hashes.empty()) return "";

    // Build the tree up until the root
    while (hashes.size() > 1) {
        std::vector<std::string> newLevel;

        for (size_t i = 0; i < hashes.size(); i += 2) {
            std::string left = hashes[i];
            std::string right = (i + 1 < hashes.size()) ? hashes[i + 1] : left;

            newLevel.push_back(doubleSHA256(left + right));
        }

        hashes = newLevel;
    }

    return hashes[0];
}
