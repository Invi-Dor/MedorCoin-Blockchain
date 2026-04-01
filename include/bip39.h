#pragma once

#include <string>
#include <vector>
#include <cstdint>

class BIP39 {
public:
    struct WalletInfo {
        std::string mnemonic;
        std::string seedHex;
        std::string derivedPrivKey;
        std::string derivedPubKey;
        std::string address;
        std::string derivationPath;
        uint32_t    accountIndex  = 0;
        uint32_t    addressIndex  = 0;
    };

    static std::string generateMnemonic();

    static std::vector<unsigned char> mnemonicToSeed(
        const std::string& mnemonic,
        const std::string& passphrase = "");

    static WalletInfo deriveFromMnemonic(
        const std::string& mnemonic,
        const std::string& passphrase   = "",
        uint32_t           account      = 0,
        uint32_t           addressIndex = 0,
        uint32_t           coinType     = 0,
        uint32_t           change       = 0,
        uint8_t            versionByte  = 0x00);

    static void wipeWalletInfo(WalletInfo& w);
    static std::string toHex(const std::vector<unsigned char>& data);
    static std::vector<std::string> loadWordList();
};
