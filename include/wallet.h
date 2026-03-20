#pragma once

#include <string>
#include <vector>

// =============================================================================
// WALLET
//
// Generates secp256k1 key pairs and MedorCoin addresses.
//
// Usage:
//   Wallet w;
//   w.generateKeys();
//   w.generateAddress();
//   std::string addr = w.getAddress();
//   w.wipe(); // always wipe when done
//
// Address format:
//   Base58Check( 0x00 || RIPEMD160( SHA256( compressedPubKey ) ) )
//   Version byte 0x00 = MedorCoin mainnet
//   Matches bip39.cpp deriveFromMnemonic() versionByte default
//
// Security:
//   - wipe() must be called when wallet is no longer needed
//   - never log or transmit privateKey or getPrivateKeyHex()
//   - generateKeys() verifies OS entropy before key generation
//   - all sensitive buffers zeroed on wipe()
// =============================================================================
class Wallet {
public:
    Wallet();

    // Deleted copy — prevent accidental key material duplication
    Wallet(const Wallet&)            = delete;
    Wallet& operator=(const Wallet&) = delete;

    // Move allowed — transfers ownership of key material
    Wallet(Wallet&&)            = default;
    Wallet& operator=(Wallet&&) = default;

    // =========================================================================
    // KEY GENERATION
    // generateKeys()    — generates secp256k1 private/public key pair
    // generateAddress() — derives MedorCoin address from public key
    // Both must be called before getters are used.
    // =========================================================================
    void generateKeys();
    void generateAddress();

    // =========================================================================
    // GETTERS
    // All throw std::runtime_error if called before generation.
    // =========================================================================
    std::string getAddress()       const;
    std::string getPublicKeyHex()  const;

    // WARNING: never log or transmit the private key in production
    std::string getPrivateKeyHex() const;

    // =========================================================================
    // WIPE
    // Securely zeros all key material from memory.
    // Always call this when wallet is no longer needed.
    // noexcept — safe to call from destructors.
    // =========================================================================
    void wipe() noexcept;

private:
    std::vector<unsigned char> privateKey;
    std::vector<unsigned char> publicKey;
    std::string                address;
};
