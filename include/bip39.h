#pragma once
#include <string>
#include <vector>

class BIP39 {
public:
    // Generates a valid BIP39 mnemonic and seed
    static std::string generateMnemonic();
    static std::vector<unsigned char> mnemonicToSeed(const std::string &mnemonic, const std::string &passphrase);

    // Derives an address & keypair from mnemonic
    struct WalletInfo {
        std::string mnemonic;
        std::string seedHex;
        std::string masterPrivKey;
        std::string masterPubKey;
        std::string address;
    };

    static WalletInfo deriveFromMnemonic(const std::string &mnemonic, const std::string &passphrase = "");

private:
    static std::vector<std::string> loadWordList();
    static std::string toHex(const std::vector<unsigned char> &data);
};
