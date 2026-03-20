#include "wallet.h"

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/sha.h>
#include <openssl/provider.h>
#include <openssl/rand.h>

#include <stdexcept>
#include <vector>
#include <string>
#include <cstring>
#include <mutex>
#include <sstream>
#include <iomanip>

// =============================================================================
// SECURE WIPE
// =============================================================================
static void secureWipe(void* ptr, size_t len) noexcept {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (len > 0) { *p = 0; ++p; --len; }
}

// =============================================================================
// OPENSSL PROVIDERS
// Fix 2: providers tracked in static pointers with atexit cleanup.
// Both legacy (RIPEMD-160) and default (SHA-256) providers loaded once.
// =============================================================================
static OSSL_PROVIDER* s_legacyProvider  = nullptr;
static OSSL_PROVIDER* s_defaultProvider = nullptr;

static void unloadWalletProviders() noexcept {
    if (s_legacyProvider) {
        OSSL_PROVIDER_unload(s_legacyProvider);
        s_legacyProvider = nullptr;
    }
    if (s_defaultProvider) {
        OSSL_PROVIDER_unload(s_defaultProvider);
        s_defaultProvider = nullptr;
    }
}

static void ensureProviders() {
    static std::once_flag flag;
    static bool loaded = false;
    std::call_once(flag, []() {
        s_legacyProvider  = OSSL_PROVIDER_load(nullptr, "legacy");
        s_defaultProvider = OSSL_PROVIDER_load(nullptr, "default");
        if (!s_legacyProvider || !s_defaultProvider) {
            unloadWalletProviders();
            throw std::runtime_error(
                "OpenSSL provider load failed — "
                "legacy and default providers required");
        }
        std::atexit(unloadWalletProviders);
        loaded = true;
    });
    if (!loaded)
        throw std::runtime_error("OpenSSL providers unavailable.");
}

// =============================================================================
// SHA-256
// =============================================================================
static std::vector<unsigned char> computeSHA256(
    const std::vector<unsigned char>& data)
{
    std::vector<unsigned char> hash(SHA256_DIGEST_LENGTH);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        throw std::runtime_error("SHA-256: EVP_MD_CTX_new failed");
    unsigned int outLen = SHA256_DIGEST_LENGTH;
    bool ok =
        (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1) &&
        (EVP_DigestUpdate(ctx, data.data(), data.size()) == 1) &&
        (EVP_DigestFinal_ex(ctx, hash.data(), &outLen) == 1);
    EVP_MD_CTX_free(ctx);
    if (!ok)
        throw std::runtime_error("SHA-256 computation failed");
    return hash;
}

// =============================================================================
// RIPEMD-160
// Requires legacy OpenSSL provider.
// =============================================================================
static std::vector<unsigned char> computeRIPEMD160(
    const std::vector<unsigned char>& data)
{
    ensureProviders();
    std::vector<unsigned char> hash(20);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        throw std::runtime_error("RIPEMD-160: EVP_MD_CTX_new failed");
    const EVP_MD* md = EVP_ripemd160();
    if (!md) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error(
            "EVP_ripemd160 returned null — "
            "is the legacy OpenSSL provider loaded?");
    }
    unsigned int outLen = 20;
    bool ok =
        (EVP_DigestInit_ex(ctx, md, nullptr) == 1) &&
        (EVP_DigestUpdate(ctx, data.data(), data.size()) == 1) &&
        (EVP_DigestFinal_ex(ctx, hash.data(), &outLen) == 1);
    EVP_MD_CTX_free(ctx);
    if (!ok)
        throw std::runtime_error("RIPEMD-160 computation failed");
    return hash;
}

// =============================================================================
// BASE58CHECK
// Fix 1: leading zeros counted from payload — not from the full buffer
//        that includes the 4-byte checksum appended after.
//        This matches Bitcoin and BIP39 address encoding exactly.
// =============================================================================
static const char BASE58_CHARS[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static std::string encodeBase58Check(
    const std::vector<unsigned char>& payload)
{
    if (payload.empty())
        throw std::invalid_argument(
            "encodeBase58Check: empty payload");

    // Double-SHA256 checksum
    auto h1 = computeSHA256(payload);
    auto h2 = computeSHA256(h1);
    secureWipe(h1.data(), h1.size());

    // Append 4-byte checksum to payload
    std::vector<unsigned char> full(payload);
    full.insert(full.end(), h2.begin(), h2.begin() + 4);
    secureWipe(h2.data(), h2.size());

    // Fix 1: count leading zero bytes from PAYLOAD only
    // not from full (which includes checksum bytes)
    size_t leadingZeros = 0;
    for (unsigned char c : payload) {
        if (c != 0) break;
        ++leadingZeros;
    }

    // Base58 encode
    std::vector<unsigned char> digits(1, 0);
    for (unsigned char byte : full) {
        unsigned int carry = byte;
        for (auto& d : digits) {
            carry += static_cast<unsigned int>(d) << 8;
            d = static_cast<unsigned char>(carry % 58);
            carry /= 58;
        }
        while (carry > 0) {
            digits.push_back(
                static_cast<unsigned char>(carry % 58));
            carry /= 58;
        }
    }

    // Build result: '1' per leading zero + base58 digits
    std::string result;
    result.reserve(leadingZeros + digits.size());
    for (size_t i = 0; i < leadingZeros; i++)
        result += '1';
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
        result += BASE58_CHARS[*it];

    secureWipe(full.data(), full.size());
    return result;
}

// =============================================================================
// HEX ENCODING HELPER
// =============================================================================
static std::string toHex(
    const std::vector<unsigned char>& data) noexcept
{
    std::ostringstream ss;
    for (unsigned char b : data)
        ss << std::hex << std::setw(2)
           << std::setfill('0')
           << static_cast<int>(b);
    return ss.str();
}

// =============================================================================
// WALLET CONSTRUCTOR
// =============================================================================
Wallet::Wallet() {}

// =============================================================================
// GENERATE KEYS
// Generates a secp256k1 private/public key pair using OS entropy.
// Private key: 32 bytes big-endian.
// Public key:  33 bytes compressed.
// =============================================================================
void Wallet::generateKeys() {
    // Verify OS entropy is available before proceeding
    unsigned char test[1];
    if (RAND_bytes(test, 1) != 1)
        throw std::runtime_error(
            "OS entropy source unavailable — cannot generate keys safely");
    secureWipe(test, 1);

    EC_KEY* ecKey = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!ecKey)
        throw std::runtime_error(
            "EC_KEY_new_by_curve_name(NID_secp256k1) failed");

    if (EC_KEY_generate_key(ecKey) != 1) {
        EC_KEY_free(ecKey);
        throw std::runtime_error("EC_KEY_generate_key failed");
    }

    // Extract private key — 32 bytes big-endian zero-padded
    const BIGNUM* priv = EC_KEY_get0_private_key(ecKey);
    if (!priv) {
        EC_KEY_free(ecKey);
        throw std::runtime_error("EC_KEY_get0_private_key returned null");
    }

    int privBytes = BN_num_bytes(priv);
    if (privBytes > 32) {
        EC_KEY_free(ecKey);
        throw std::runtime_error(
            "Private key exceeds 32 bytes — invalid key");
    }

    privateKey.assign(32, 0);
    BN_bn2bin(priv, privateKey.data() + (32 - privBytes));

    // Extract public key — 33 bytes compressed
    EC_KEY_set_conv_form(ecKey, POINT_CONVERSION_COMPRESSED);
    int pubLen = i2o_ECPublicKey(ecKey, nullptr);
    if (pubLen <= 0) {
        EC_KEY_free(ecKey);
        throw std::runtime_error(
            "i2o_ECPublicKey size query failed");
    }
    publicKey.resize(static_cast<size_t>(pubLen));
    unsigned char* pubPtr = publicKey.data();
    if (i2o_ECPublicKey(ecKey, &pubPtr) != pubLen) {
        EC_KEY_free(ecKey);
        throw std::runtime_error(
            "i2o_ECPublicKey serialisation failed");
    }
    EC_KEY_free(ecKey);
}

// =============================================================================
// GENERATE ADDRESS
// Fix 3: version byte 0x00 matches bip39.cpp default versionByte.
// Address = Base58Check( 0x00 || RIPEMD160( SHA256( compressedPubKey ) ) )
// This produces a standard Bitcoin-style P2PKH address compatible
// with MedorCoin mainnet (version byte 0x00).
// =============================================================================
void Wallet::generateAddress() {
    if (publicKey.empty())
        throw std::runtime_error(
            "No public key — call generateKeys() before generateAddress()");
    if (publicKey.size() != 33)
        throw std::runtime_error(
            "Public key must be 33 bytes compressed — "
            "call generateKeys() to generate a valid key");

    // SHA-256 of compressed public key
    auto sha = computeSHA256(publicKey);

    // RIPEMD-160 of SHA-256 result
    auto rip = computeRIPEMD160(sha);
    secureWipe(sha.data(), sha.size());

    // Version byte 0x00 = MedorCoin mainnet (matches bip39.cpp)
    std::vector<unsigned char> versioned;
    versioned.reserve(21);
    versioned.push_back(0x00);
    versioned.insert(versioned.end(), rip.begin(), rip.end());
    secureWipe(rip.data(), rip.size());

    // Base58Check encode
    address = encodeBase58Check(versioned);
    secureWipe(versioned.data(), versioned.size());
}

// =============================================================================
// GET PUBLIC KEY HEX
// =============================================================================
std::string Wallet::getPublicKeyHex() const {
    if (publicKey.empty())
        throw std::runtime_error(
            "No public key — call generateKeys() first");
    return toHex(publicKey);
}

// =============================================================================
// GET PRIVATE KEY HEX
// WARNING: never log or transmit the private key in production
// =============================================================================
std::string Wallet::getPrivateKeyHex() const {
    if (privateKey.empty())
        throw std::runtime_error(
            "No private key — call generateKeys() first");
    return toHex(privateKey);
}

// =============================================================================
// GET ADDRESS
// =============================================================================
std::string Wallet::getAddress() const {
    if (address.empty())
        throw std::runtime_error(
            "No address — call generateAddress() first");
    return address;
}

// =============================================================================
// WIPE
// Securely clears all key material from memory.
// Call this when the wallet is no longer needed.
// =============================================================================
void Wallet::wipe() noexcept {
    if (!privateKey.empty()) {
        secureWipe(privateKey.data(), privateKey.size());
        privateKey.clear();
    }
    if (!publicKey.empty()) {
        secureWipe(publicKey.data(), publicKey.size());
        publicKey.clear();
    }
    if (!address.empty()) {
        secureWipe(&address[0], address.size());
        address.clear();
    }
}
