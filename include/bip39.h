#pragma once

#include <string>
#include <vector>
#include <cstdint>

class BIP39 {
public:

    struct WalletInfo {
        std::string mnemonic;
        std::string seedHex;
        std::string masterPrivKey;
        std::string masterPubKey;
        std::string address;
        std::string derivationPath;
        uint32_t    accountIndex;
        uint32_t    addressIndex;
    };

    // Generates a BIP39-compliant 12-word mnemonic from 128-bit entropy.
    // Verifies its own checksum before returning.
    static std::string generateMnemonic();

    // Derives a 64-byte seed from a mnemonic via PBKDF2-HMAC-SHA512.
    // Validates the BIP39 checksum of the mnemonic before proceeding.
    static std::vector<unsigned char> mnemonicToSeed(
        const std::string& mnemonic,
        const std::string& passphrase = "");

    // Full BIP44 key and address derivation from a mnemonic.
    // Default path: m/44'/0'/account'/0/addressIndex
    // Coin type 0 = MedorCoin mainnet.
    static WalletInfo deriveFromMnemonic(
        const std::string& mnemonic,
        const std::string& passphrase  = "",
        uint32_t           account     = 0,
        uint32_t           addressIndex = 0);

    // Converts a byte vector to a lowercase hex string.
    static std::string toHex(const std::vector<unsigned char>& data);

    // Loads and returns the 2048-word BIP39 English wordlist.
    // The list is cached after the first call.
    static std::vector<std::string> loadWordList();

    // Verifies that a mnemonic string passes the BIP39 checksum.
    // Returns true if valid, false otherwise.
    static bool validateMnemonic(const std::string& mnemonic);
};
