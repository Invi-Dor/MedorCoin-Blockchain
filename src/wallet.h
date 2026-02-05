#pragma once

#include <string>
#include <vector>

class Wallet {
public:
    std::vector<unsigned char> privateKey;
    std::vector<unsigned char> publicKey;
    std::string address;

    Wallet();
    void generateKeys();
    void generateAddress();  // Bitcoin‑style Base58Check
};
