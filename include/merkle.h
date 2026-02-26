#ifndef MERKLE_H
#define MERKLE_H

#include <vector>
#include <string>
#include "transaction.h"

class MerkleTree {
public:
    std::string root;

    MerkleTree(const std::vector<Transaction>& transactions);

private:
    std::string computeMerkleRoot(std::vector<std::string> hashes);
};

#endif
