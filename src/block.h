#ifndef BLOCK_H
#define BLOCK_H

#include <string>
#include <cstdint>

class Block {
public:
    int version;
    std::string previousHash;
    std::string merkleRoot;
    uint32_t timestamp;
    uint32_t medor;
    uint32_t nonce;
    std::string hash;

    Block(std::string prevHash, std::string data, uint32_t medorVal);

    std::string headerToString();
    std::string mine();
};

std::string doubleSHA256(const std::string& input);
std::string medorToTarget(uint32_t medor);

#endif
