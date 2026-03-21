#pragma once

#include <string>
#include <vector>

// =============================================================================
// WALLET
//
// Generates secp256k1 key pairs and MedorCoin addresses.
// Key generation uses crypto::generateKeypair() from secp256k1_wrapper.
// The deprecated OpenSSL EC_KEY API is permanently banned from this file.
// Use crypto::generateKeypair() for all key generation -- never EC_KEY.
//
// Usage:
//   Wallet w;
//   w.generateKeys();
//   w.generateAddress();
//   std::string addr = w.getAddress();
//   w.wipe();
//
// Address format:
//   Base58Check( 0x00 || RIPEMD160( SHA256( compressedPubKey ) ) )
//   Version byte 0x00 = MedorCoin mainnet
//   Matches bip39.cpp deriveFromMnemonic() versionByte default
//
// Security:
//   - wipe() must be called when wallet is no longer needed
//   - never log or transmit privateKey or getPrivateKeyHex()
//   - generateKeys() uses libsecp256k1 via crypto::generateKeypair()
//   - all sensitive buffers zeroed on wipe()
// =============================================================================

// =============================================================================
// DEPRECATION GUARD
// The OpenSSL EC_KEY API is deprecated in OpenSSL 3.x and permanently
// removed from this project. The static_asserts below will break the build
// if anyone attempts to re-introduce these deprecated symbols anywhere
// in this translation unit.
// =============================================================================
#ifdef EC_KEY_new_by_curve_name
static_assert(false,
    "EC_KEY_new_by_curve_name is deprecated and banned. "
    "Use crypto::generateKeypair() from crypto/secp256k1_wrapper.h instead.");
#endif
#ifdef i2o_ECPublicKey
static_assert(false,
    "i2o_ECPublicKey is deprecated and banned. "
    "Use crypto::generateKeypair() from crypto/secp256k1_wrapper.h instead.");
#endif
#ifdef EC_KEY_generate_key
static_assert(false,
    "EC_KEY_generate_key is deprecated and banned. "
    "Use crypto::generateKeypair() from crypto/secp256k1_wrapper.h instead.");
#endif
#ifdef EC_KEY_free
static_assert(false,
    "EC_KEY_free is deprecated and banned. "
    "Use crypto::generateKeypair() from crypto/secp256k1_wrapper.h instead.");
#endif

class Wallet {
public:
    Wallet();

    Wallet(const Wallet&)            = delete;
    Wallet& operator=(const Wallet&) = delete;

    Wallet(Wallet&&)            = default;
    Wallet& operator=(Wallet&&) = default;

    void generateKeys();
    void generateAddress();

    std::string getAddress()       const;
    std::string getPublicKeyHex()  const;
    std::string getPrivateKeyHex() const;

    void wipe() noexcept;

private:
    std::vector<unsigned char> privateKey;
    std::vector<unsigned char> publicKey;
    std::string                address;
};
