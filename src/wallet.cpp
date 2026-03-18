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

static void secureWipe(void* ptr, size_t len) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (len > 0) { *p = 0; ++p; len = len - 1; }
}

static void ensureProviders() {
    static std::once_flag flag;
    static bool loaded = false;
    std::call_once(flag, []() {
        OSSL_PROVIDER* legacy = OSSL_PROVIDER_load(nullptr, "legacy");
        OSSL_PROVIDER* dflt   = OSSL_PROVIDER_load(nullptr, "default");
        if (!legacy || !dflt)
            throw std::runtime_error("OpenSSL provider load failed.");
        loaded = true;
    });
    if (!loaded)
        throw std::runtime_error("OpenSSL providers unavailable.");
}

static std::vector<unsigned char> computeSHA256(
    const std::vector<unsigned char>& data)
{
    std::vector<unsigned char> hash(SHA256_DIGEST_LENGTH);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("SHA-256: EVP_MD_CTX_new failed");
    unsigned int outLen = SHA256_DIGEST_LENGTH;
    bool ok = (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1) &&
              (EVP_DigestUpdate(ctx, data.data(), data.size()) == 1) &&
              (EVP_DigestFinal_ex(ctx, hash.data(), &outLen)   == 1);
    EVP_MD_CTX_free(ctx);
    if (!ok) throw std::runtime_error("SHA-256 computation failed");
    return hash;
}

static std::vector<unsigned char> computeRIPEMD160(
    const std::vector<unsigned char>& data)
{
    ensureProviders();
    std::vector<unsigned char> hash(20);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("RIPEMD-160: EVP_MD_CTX_new failed");
    const EVP_MD* md = EVP_ripemd160();
    if (!md) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_ripemd160 returned null.");
    }
    unsigned int outLen = 20;
    bool ok = (EVP_DigestInit_ex(ctx, md, nullptr)              == 1) &&
              (EVP_DigestUpdate(ctx, data.data(), data.size())   == 1) &&
              (EVP_DigestFinal_ex(ctx, hash.data(), &outLen)     == 1);
    EVP_MD_CTX_free(ctx);
    if (!ok) throw std::runtime_error("RIPEMD-160 computation failed");
    return hash;
}

static const char BASE58_CHARS[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static std::string encodeBase58Check(
    const std::vector<unsigned char>& payload)
{
    auto h1 = computeSHA256(payload);
    auto h2 = computeSHA256(h1);
    std::vector<unsigned char> full(payload);
    full.insert(full.end(), h2.begin(), h2.begin() + 4);
    std::vector<unsigned char> digits(1, 0);
    for (unsigned char byte : full) {
        unsigned int carry = byte;
        for (auto& d : digits) {
            carry += static_cast<unsigned int>(d) << 8;
            d = static_cast<unsigned char>(carry % 58);
            carry /= 58;
        }
        while (carry) {
            digits.push_back(static_cast<unsigned char>(carry % 58));
            carry /= 58;
        }
    }
    std::string result;
    for (unsigned char c : full) {
        if (c != 0) break;
        result += '1';
    }
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
        result += BASE58_CHARS[*it];
    return result;
}

Wallet::Wallet() {}

void Wallet::generateKeys() {
    unsigned char test[1];
    if (RAND_bytes(test, 1) != 1)
        throw std::runtime_error("OS entropy source unavailable.");

    EC_KEY* ecKey = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!ecKey)
        throw std::runtime_error("EC_KEY_new_by_curve_name failed");
    if (EC_KEY_generate_key(ecKey) != 1) {
        EC_KEY_free(ecKey);
        throw std::runtime_error("EC_KEY_generate_key failed");
    }

    const BIGNUM* priv = EC_KEY_get0_private_key(ecKey);
    if (!priv) {
        EC_KEY_free(ecKey);
        throw std::runtime_error("EC_KEY_get0_private_key failed");
    }
    privateKey.resize(32, 0);
    int privBytes = BN_num_bytes(priv);
    if (privBytes > 32) {
        EC_KEY_free(ecKey);
        throw std::runtime_error("Private key exceeds 32 bytes");
    }
    BN_bn2bin(priv, privateKey.data() + (32 - privBytes));

    EC_KEY_set_conv_form(ecKey, POINT_CONVERSION_COMPRESSED);
    int pubLen = i2o_ECPublicKey(ecKey, nullptr);
    if (pubLen <= 0) {
        EC_KEY_free(ecKey);
        throw std::runtime_error("i2o_ECPublicKey size query failed");
    }
    publicKey.resize(pubLen);
    unsigned char* pubPtr = publicKey.data();
    if (i2o_ECPublicKey(ecKey, &pubPtr) != pubLen) {
        EC_KEY_free(ecKey);
        throw std::runtime_error("i2o_ECPublicKey serialisation failed");
    }
    EC_KEY_free(ecKey);
}

void Wallet::generateAddress() {
    if (publicKey.empty())
        throw std::runtime_error("No public key: call generateKeys() first");

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
