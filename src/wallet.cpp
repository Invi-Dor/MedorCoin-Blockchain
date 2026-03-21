// =============================================================================
// DEPRECATION GUARD
// The OpenSSL EC_KEY API is deprecated in OpenSSL 3.x and permanently
// banned from this file. If any of the symbols below are defined it means
// a deprecated header was included and the build must fail immediately.
// All key generation must go through crypto::generateKeypair().
// =============================================================================
#ifdef EC_KEY_new_by_curve_name
static_assert(false,
    "EC_KEY_new_by_curve_name is banned. Use crypto::generateKeypair().");
#endif
#ifdef i2o_ECPublicKey
static_assert(false,
    "i2o_ECPublicKey is banned. Use crypto::generateKeypair().");
#endif
#ifdef EC_KEY_generate_key
static_assert(false,
    "EC_KEY_generate_key is banned. Use crypto::generateKeypair().");
#endif

#include "wallet.h"
#include "crypto/secp256k1_wrapper.h"
#include "crypto/openssl_providers.h"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <stdexcept>
#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include <iomanip>

static void secureWipe(void* ptr, size_t len) noexcept {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (len > 0) { *p = 0; ++p; --len; }
}

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

static std::vector<unsigned char> computeRIPEMD160(
    const std::vector<unsigned char>& data)
{
    crypto::ensureOpenSSLProviders();
    std::vector<unsigned char> hash(20);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        throw std::runtime_error("RIPEMD-160: EVP_MD_CTX_new failed");
    const EVP_MD* md = EVP_ripemd160();
    if (!md) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error(
            "EVP_ripemd160 returned null -- legacy OpenSSL provider not loaded");
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

static const char BASE58_CHARS[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static std::string encodeBase58Check(
    const std::vector<unsigned char>& payload)
{
    if (payload.empty())
        throw std::invalid_argument("encodeBase58Check: empty payload");
    auto h1 = computeSHA256(payload);
    auto h2 = computeSHA256(h1);
    secureWipe(h1.data(), h1.size());
    std::vector<unsigned char> full(payload);
    full.insert(full.end(), h2.begin(), h2.begin() + 4);
    secureWipe(h2.data(), h2.size());
    size_t leadingZeros = 0;
    for (unsigned char c : payload) {
        if (c != 0) break;
        ++leadingZeros;
    }
    std::vector<unsigned char> digits(1, 0);
    for (unsigned char byte : full) {
        unsigned int carry = byte;
        for (auto& d : digits) {
            carry += static_cast<unsigned int>(d) << 8;
            d = static_cast<unsigned char>(carry % 58);
            carry /= 58;
        }
        while (carry > 0) {
            digits.push_back(static_cast<unsigned char>(carry % 58));
            carry /= 58;
        }
    }
    std::string result;
    result.reserve(leadingZeros + digits.size());
    for (size_t i = 0; i < leadingZeros; i++) result += '1';
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
        result += BASE58_CHARS[*it];
    secureWipe(full.data(), full.size());
    return result;
}

static std::string toHex(
    const std::vector<unsigned char>& data) noexcept
{
    std::ostringstream ss;
    for (unsigned char b : data)
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(b);
    return ss.str();
}

Wallet::Wallet() {}

void Wallet::generateKeys()
{
    auto kp = crypto::generateKeypair();
    if (!kp)
        throw std::runtime_error(
            "generateKeys: secp256k1 keypair generation failed");
    privateKey.assign(kp->privkey.begin(), kp->privkey.end());
    publicKey.assign(kp->pubkey_compressed.begin(),
                     kp->pubkey_compressed.end());
    kp->privkey.fill(0);
}

void Wallet::generateAddress()
{
    if (publicKey.empty())
        throw std::runtime_error(
            "No public key -- call generateKeys() before generateAddress()");
    if (publicKey.size() != 33)
        throw std::runtime_error(
            "Public key must be 33 bytes compressed");
    auto sha = computeSHA256(publicKey);
    auto rip = computeRIPEMD160(sha);
    secureWipe(sha.data(), sha.size());
    std::vector<unsigned char> versioned;
    versioned.reserve(21);
    versioned.push_back(0x00);
    versioned.insert(versioned.end(), rip.begin(), rip.end());
    secureWipe(rip.data(), rip.size());
    address = encodeBase58Check(versioned);
    secureWipe(versioned.data(), versioned.size());
}

std::string Wallet::getPublicKeyHex() const
{
    if (publicKey.empty())
        throw std::runtime_error(
            "No public key -- call generateKeys() first");
    return toHex(publicKey);
}

std::string Wallet::getPrivateKeyHex() const
{
    if (privateKey.empty())
        throw std::runtime_error(
            "No private key -- call generateKeys() first");
    return toHex(privateKey);
}

std::string Wallet::getAddress() const
{
    if (address.empty())
        throw std::runtime_error(
            "No address -- call generateAddress() first");
    return address;
}

void Wallet::wipe() noexcept
{
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
