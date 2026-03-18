#pragma once

#include <string>
#include <vector>
#include <cstdint>

class Wallet {
public:

    std::vector<unsigned char> privateKey;
    std::vector<unsigned char> publicKey;
    std::string                address;

    Wallet();

    // Generates a new secp256k1 private/public key pair using OS entropy.
    // Stores a compressed 33-byte public key.
    // Throws std::runtime_error on failure.
    void generateKeys();

    // Derives a MedorCoin P2PKH address from the current public key.
    // Uses HASH160 (SHA-256 then RIPEMD-160) and Base58Check encoding.
    // Version byte 0x00 = MedorCoin mainnet.
    // Throws std::runtime_error if no public key is present.
    void generateAddress();

    // Wipes the private key from memory securely.
    // Call this when the Wallet object is no longer needed
    // and the private key should not remain in process memory.
    void clearPrivateKey();

    // Returns true if the wallet holds a valid key pair.
    bool isInitialised() const;
};
