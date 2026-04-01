#include "bip39.h"
#include "wallet.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/provider.h>
#include <secp256k1.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <stdexcept>
#include <mutex>
#include <thread>
#include <climits>
#include <algorithm>
#include <cstdlib>

static_assert(CHAR_BIT == 8, "This code requires 8-bit bytes");

static constexpr size_t MAX_MNEMONIC_LEN   = 1024;
static constexpr size_t MAX_PASSPHRASE_LEN = 512;
static constexpr size_t BIP39_MIN_WORDS    = 1;

static void checkLength(
    const std::string& s, size_t limit, const char* name)
{
    if (s.size() > limit)
        throw std::invalid_argument(
            std::string(name) + " exceeds maximum length of "
            + std::to_string(limit) + " bytes");
}

static void secureWipe(void* ptr, size_t len) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (len > 0) { *p = 0; ++p; len = len - 1; }
}

static void secureWipeString(std::string& s) {
    if (!s.empty()) {
        secureWipe(&s[0], s.size());
        s.clear();
    }
}

template<size_t N>
struct SecureBuffer {
    unsigned char data[N];
    SecureBuffer()  { memset(data, 0, N); }
    ~SecureBuffer() { secureWipe(data, N); }
    SecureBuffer(const SecureBuffer&)            = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;
    SecureBuffer(SecureBuffer&& o) noexcept {
        memcpy(data, o.data, N);
        secureWipe(o.data, N);
    }
    SecureBuffer& operator=(SecureBuffer&& o) noexcept {
        if (this != &o) {
            memcpy(data, o.data, N);
            secureWipe(o.data, N);
        }
        return *this;
    }
};

// =============================================================================
// SECP256K1 CONTEXT
// Created once, destroyed at process exit via atexit.
// =============================================================================
static secp256k1_context* g_secp256k1_ctx = nullptr;

static void destroyCtx() {
    if (g_secp256k1_ctx) {
        secp256k1_context_destroy(g_secp256k1_ctx);
        g_secp256k1_ctx = nullptr;
    }
}

static secp256k1_context* getCtx() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        g_secp256k1_ctx = secp256k1_context_create(
            SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
        if (!g_secp256k1_ctx)
            throw std::runtime_error("secp256k1_context_create failed");
        std::atexit(destroyCtx);
    });
    return g_secp256k1_ctx;
}

// =============================================================================
// OPENSSL PROVIDERS
// Loaded once, unloaded at process exit via atexit.
// =============================================================================
static OSSL_PROVIDER* g_legacy_provider  = nullptr;
static OSSL_PROVIDER* g_default_provider = nullptr;

static void unloadProviders() {
    if (g_legacy_provider) {
        OSSL_PROVIDER_unload(g_legacy_provider);
        g_legacy_provider = nullptr;
    }
    if (g_default_provider) {
        OSSL_PROVIDER_unload(g_default_provider);
        g_default_provider = nullptr;
    }
}

static void ensureProviders() {
    static std::once_flag flag;
    static bool loaded = false;
    std::call_once(flag, []() {
        g_legacy_provider  = OSSL_PROVIDER_load(nullptr, "legacy");
        g_default_provider = OSSL_PROVIDER_load(nullptr, "default");
        if (!g_legacy_provider || !g_default_provider) {
            unloadProviders();
            throw std::runtime_error("OpenSSL provider load failed.");
        }
        std::atexit(unloadProviders);
        loaded = true;
    });
    if (!loaded)
        throw std::runtime_error("OpenSSL providers unavailable.");
}

// =============================================================================
// WORDLIST
// =============================================================================
static const std::vector<std::string>& getWordList() {
    static std::vector<std::string> cache;
    static std::once_flag flag;
    std::call_once(flag, []() {
        std::vector<std::string> candidates = {
            "bip39_wordlist.txt",
            "/usr/share/medorcoin/bip39_wordlist.txt",
            "/etc/medorcoin/bip39_wordlist.txt"
        };
        const char* envPath = std::getenv("MEDOR_WORDLIST_PATH");
        if (envPath)
            candidates.insert(candidates.begin(), std::string(envPath));
        for (const auto& path : candidates) {
            std::ifstream f(path);
            if (!f.is_open()) continue;
            std::string word;
            while (f >> word) cache.push_back(word);
            if (cache.size() < BIP39_MIN_WORDS) {
                cache.clear();
                continue;
            }
            return;
        }
        throw std::runtime_error(
            "BIP39 wordlist not found or empty. "
            "Set MEDOR_WORDLIST_PATH or place bip39_wordlist.txt "
            "in the working directory.");
    });
    if (cache.empty())
        throw std::runtime_error("BIP39 wordlist is empty.");
    return cache;
}

static void sha256(
    const unsigned char* in, size_t inLen,
    unsigned char out[SHA256_DIGEST_LENGTH])
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("SHA-256: EVP_MD_CTX_new failed");
    unsigned int outLen = SHA256_DIGEST_LENGTH;
    bool ok = (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1) &&
              (EVP_DigestUpdate(ctx, in, inLen)               == 1) &&
              (EVP_DigestFinal_ex(ctx, out, &outLen)          == 1);
    EVP_MD_CTX_free(ctx);
    if (!ok) throw std::runtime_error("SHA-256 computation failed");
}

static void ripemd160(
    const unsigned char* in, size_t inLen,
    unsigned char out[20])
{
    ensureProviders();
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("RIPEMD-160: EVP_MD_CTX_new failed");
    const EVP_MD* md = EVP_ripemd160();
    if (!md) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_ripemd160 returned null.");
    }
    unsigned int outLen = 20;
    bool ok = (EVP_DigestInit_ex(ctx, md, nullptr) == 1) &&
              (EVP_DigestUpdate(ctx, in, inLen)     == 1) &&
              (EVP_DigestFinal_ex(ctx, out, &outLen) == 1);
    EVP_MD_CTX_free(ctx);
    if (!ok) throw std::runtime_error("RIPEMD-160 computation failed");
}

static const char BASE58_CHARS[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static std::string base58check(const std::vector<unsigned char>& payload) {
    if (payload.empty())
        throw std::invalid_argument("base58check: empty payload");

    unsigned char h1[SHA256_DIGEST_LENGTH], h2[SHA256_DIGEST_LENGTH];
    sha256(payload.data(), payload.size(), h1);
    sha256(h1, SHA256_DIGEST_LENGTH, h2);

    std::vector<unsigned char> full(payload);
    full.insert(full.end(), h2, h2 + 4);

    // Count leading zero bytes in the original payload only
    size_t leadingZeros = 0;
    for (size_t i = 0; i < payload.size(); i++) {
        if (payload[i] != 0) break;
        leadingZeros++;
    }

    std::vector<unsigned char> digits(1, 0);
    for (size_t i = 0; i < full.size(); i++) {
        unsigned int carry = full[i];
        for (size_t j = 0; j < digits.size(); j++) {
            carry += static_cast<unsigned int>(digits[j]) << 8;
            digits[j] = static_cast<unsigned char>(carry % 58);
            carry /= 58;
        }
        while (carry > 0) {
            digits.push_back(static_cast<unsigned char>(carry % 58));
            carry /= 58;
        }
    }

    std::string result;
    for (size_t i = 0; i < leadingZeros; i++)
        result += '1';
    for (size_t i = digits.size(); i > 0; i--)
        result += BASE58_CHARS[digits[i - 1]];
    return result;
}

// =============================================================================
// CHECKSUM VERIFICATION
// Supports all five BIP39 word counts: 12, 15, 18, 21, 24.
// =============================================================================
static bool verifyChecksum(const std::string& mnemonic) {
    const auto& words = getWordList();
    std::vector<std::string> tokens;
    std::istringstream iss(mnemonic);
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);

    size_t wordCount    = tokens.size();
    size_t entropyBytes = 0;
    size_t checksumBits = 0;
    if      (wordCount == 12) { entropyBytes = 16; checksumBits = 4; }
    else if (wordCount == 15) { entropyBytes = 20; checksumBits = 5; }
    else if (wordCount == 18) { entropyBytes = 24; checksumBits = 6; }
    else if (wordCount == 21) { entropyBytes = 28; checksumBits = 7; }
    else if (wordCount == 24) { entropyBytes = 32; checksumBits = 8; }
    else return false;

    size_t totalBits  = wordCount * 11;
    size_t totalBytes = (totalBits + 7) / 8;
    std::vector<unsigned char> bits(totalBytes, 0);

    for (size_t i = 0; i < wordCount; i++) {
        auto it = std::find(words.begin(), words.end(), tokens[i]);
        if (it == words.end()) return false;
        unsigned int idx = static_cast<unsigned int>(it - words.begin());
        for (int b = 10; b >= 0; b--) {
            size_t bitPos = i * 11 + static_cast<size_t>(10 - b);
            if (idx & (1u << static_cast<unsigned>(b)))
                bits[bitPos / 8] |=
                    static_cast<unsigned char>(1 << (7 - (bitPos % 8)));
        }
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    sha256(bits.data(), entropyBytes, hash);

    bool valid = true;
    for (size_t i = 0; i < checksumBits; i++) {
        size_t bitPos = entropyBytes * 8 + i;
        bool expected = (hash[i / 8] >> (7 - (i % 8))) & 1;
        bool actual   = (bits[bitPos / 8] >> (7 - (bitPos % 8))) & 1;
        if (expected != actual) { valid = false; break; }
    }

    secureWipe(bits.data(), bits.size());
    secureWipe(hash, sizeof(hash));
    return valid;
}

struct MasterKey {
    SecureBuffer<32> privKey;
    SecureBuffer<32> chainCode;
    MasterKey() = default;
    MasterKey(MasterKey&&) = default;
    MasterKey& operator=(MasterKey&&) = default;
};

static MasterKey deriveMaster(const std::vector<unsigned char>& seed) {
    if (seed.size() < 16 || seed.size() > 64)
        throw std::invalid_argument("deriveMaster: seed length out of range");

    const unsigned char hmacKey[] = "Bitcoin seed";
    unsigned char out[64];
    unsigned int  outLen = 64;
    if (!HMAC(EVP_sha512(),
              hmacKey, static_cast<int>(sizeof(hmacKey) - 1),
              seed.data(), static_cast<int>(seed.size()),
              out, &outLen) || outLen != 64) {
        secureWipe(out, sizeof(out));
        throw std::runtime_error("BIP32 HMAC-SHA512 derivation failed");
    }
    MasterKey mk;
    memcpy(mk.privKey.data,   out,      32);
    memcpy(mk.chainCode.data, out + 32, 32);
    secureWipe(out, sizeof(out));
    if (secp256k1_ec_seckey_verify(getCtx(), mk.privKey.data) != 1)
        throw std::runtime_error("BIP32 master key outside curve range.");
    return mk;
}

struct ChildKey {
    SecureBuffer<32> privKey;
    SecureBuffer<32> chainCode;
    ChildKey() = default;
    ChildKey(ChildKey&&) = default;
    ChildKey& operator=(ChildKey&&) = default;
};

static ChildKey deriveChild(
    const unsigned char parentPriv[32],
    const unsigned char parentChain[32],
    unsigned int index)
{
    if (!parentPriv)
        throw std::invalid_argument("deriveChild: null parentPriv");
    if (!parentChain)
        throw std::invalid_argument("deriveChild: null parentChain");
    if (secp256k1_ec_seckey_verify(getCtx(), parentPriv) != 1)
        throw std::invalid_argument(
            "deriveChild: parentPriv is not a valid secp256k1 key");

    unsigned char data[37];
    unsigned char out[64];
    unsigned int  outLen = 64;
    bool hardened = (index >= 0x80000000u);

    if (hardened) {
        // Hardened: 0x00 || parentPriv || ser32(index)
        data[0] = 0x00;
        memcpy(data + 1, parentPriv, 32);
    } else {
        // Normal: compressed parentPub || ser32(index)
        secp256k1_pubkey pub;
        if (secp256k1_ec_pubkey_create(getCtx(), &pub, parentPriv) != 1) {
            secureWipe(data, sizeof(data));
            throw std::runtime_error("Child key pubkey creation failed");
        }
        size_t pubLen = 33;
        if (secp256k1_ec_pubkey_serialize(
                getCtx(), data, &pubLen, &pub,
                SECP256K1_EC_COMPRESSED) != 1 || pubLen != 33) {
            secureWipe(data, sizeof(data));
            throw std::runtime_error(
                "Child key pubkey serialization failed");
        }
    }

    data[33] = static_cast<unsigned char>((index >> 24) & 0xFF);
    data[34] = static_cast<unsigned char>((index >> 16) & 0xFF);
    data[35] = static_cast<unsigned char>((index >>  8) & 0xFF);
    data[36] = static_cast<unsigned char>( index        & 0xFF);

    if (!HMAC(EVP_sha512(),
              parentChain, 32,
              data, 37,
              out, &outLen) || outLen != 64) {
        secureWipe(out, sizeof(out));
        secureWipe(data, sizeof(data));
        throw std::runtime_error("Child key HMAC-SHA512 failed");
    }
    secureWipe(data, sizeof(data));

    ChildKey ck;
    memcpy(ck.privKey.data,   out,      32);
    memcpy(ck.chainCode.data, out + 32, 32);
    secureWipe(out, sizeof(out));

    // BIP32: child = (IL + parentPriv) mod n for both hardened and normal
    if (secp256k1_ec_seckey_tweak_add(
            getCtx(), ck.privKey.data, parentPriv) != 1)
        throw std::runtime_error("Child key tweak_add failed.");
    if (secp256k1_ec_seckey_verify(getCtx(), ck.privKey.data) != 1)
        throw std::runtime_error("Child key outside curve range.");
    return ck;
}

std::vector<std::string> BIP39::loadWordList() {
    return getWordList();
}

std::string BIP39::generateMnemonic() {
    const auto& words = getWordList();
    SecureBuffer<16> entropy;
    if (RAND_bytes(entropy.data, 16) != 1)
        throw std::runtime_error("RAND_bytes failed.");
    SecureBuffer<SHA256_DIGEST_LENGTH> hash;
    sha256(entropy.data, 16, hash.data);
    SecureBuffer<17> bits;
    memcpy(bits.data, entropy.data, 16);
    bits.data[16] = hash.data[0];
    std::string mnemonic;
    mnemonic.reserve(132);
    for (int i = 0; i < 12; i++) {
        int bitOff  = i * 11;
        int byteIdx = bitOff / 8;
        int bitPos  = bitOff % 8;
        unsigned int value = 0;
        value |= static_cast<unsigned int>(bits.data[byteIdx])     << 16;
        value |= static_cast<unsigned int>(bits.data[byteIdx + 1]) <<  8;
        if (byteIdx + 2 < 17)
            value |= static_cast<unsigned int>(bits.data[byteIdx + 2]);
        value = (value >> (13 - bitPos)) & 0x7FFu;
        if (value >= words.size())
            throw std::runtime_error(
                "generateMnemonic: word index out of range");
        mnemonic += words[value];
        if (i < 11) mnemonic += ' ';
    }
    if (!verifyChecksum(mnemonic))
        throw std::runtime_error("Mnemonic checksum self-verification failed.");
    return mnemonic;
}

std::vector<unsigned char> BIP39::mnemonicToSeed(
    const std::string& mnemonic,
    const std::string& passphrase)
{
    checkLength(mnemonic,   MAX_MNEMONIC_LEN,   "mnemonic");
    checkLength(passphrase, MAX_PASSPHRASE_LEN, "passphrase");
    if (!verifyChecksum(mnemonic))
        throw std::invalid_argument("Mnemonic BIP39 checksum failed.");
    std::string salt = "mnemonic" + passphrase;
    std::vector<unsigned char> seed(64);
    if (PKCS5_PBKDF2_HMAC(
            mnemonic.c_str(),  static_cast<int>(mnemonic.size()),
            reinterpret_cast<const unsigned char*>(salt.c_str()),
            static_cast<int>(salt.size()),
            2048, EVP_sha512(), 64, seed.data()) != 1) {
        secureWipe(seed.data(), seed.size());
        secureWipeString(salt);
        throw std::runtime_error("PBKDF2-HMAC-SHA512 failed");
    }
    secureWipeString(salt);
    return seed;
}

std::string BIP39::toHex(const std::vector<unsigned char>& data) {
    std::ostringstream ss;
    for (unsigned char byte : data)
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(byte);
    return ss.str();
}

BIP39::WalletInfo BIP39::deriveFromMnemonic(
    const std::string& mnemonic,
    const std::string& passphrase,
    uint32_t           account,
    uint32_t           addressIndex,
    uint32_t           coinType,
    uint32_t           change,
    uint8_t            versionByte)
{
    checkLength(mnemonic,   MAX_MNEMONIC_LEN,   "mnemonic");
    checkLength(passphrase, MAX_PASSPHRASE_LEN, "passphrase");

    WalletInfo w;
    w.mnemonic      = mnemonic;
    w.accountIndex  = account;
    w.addressIndex  = addressIndex;
    w.derivationPath =
        "m/44'/" + std::to_string(coinType) + "'/" +
        std::to_string(account) + "'/" +
        std::to_string(change)  + "/" +
        std::to_string(addressIndex);

    auto seed = mnemonicToSeed(mnemonic, passphrase);
    w.seedHex = toHex(seed);

    MasterKey master = deriveMaster(seed);
    secureWipe(seed.data(), seed.size());

    ChildKey lvl1 = deriveChild(
        master.privKey.data, master.chainCode.data, 0x8000002Cu);
    ChildKey lvl2 = deriveChild(
        lvl1.privKey.data, lvl1.chainCode.data, coinType | 0x80000000u);
    ChildKey lvl3 = deriveChild(
        lvl2.privKey.data, lvl2.chainCode.data, account | 0x80000000u);
    ChildKey lvl4 = deriveChild(
        lvl3.privKey.data, lvl3.chainCode.data, change);
    ChildKey lvl5 = deriveChild(
        lvl4.privKey.data, lvl4.chainCode.data, addressIndex);

    secp256k1_pubkey pubkey;
    if (secp256k1_ec_pubkey_create(
            getCtx(), &pubkey, lvl5.privKey.data) != 1)
        throw std::runtime_error("secp256k1 pubkey creation failed");

    unsigned char pubComp[33];
    size_t pubCompLen = 33;
    if (secp256k1_ec_pubkey_serialize(
            getCtx(), pubComp, &pubCompLen,
            &pubkey, SECP256K1_EC_COMPRESSED) != 1 || pubCompLen != 33) {
        secureWipe(pubComp, sizeof(pubComp));
        throw std::runtime_error("Pubkey serialization failed");
    }

    w.derivedPrivKey = toHex({lvl5.privKey.data, lvl5.privKey.data + 32});
    w.derivedPubKey  = toHex({pubComp, pubComp + pubCompLen});

    unsigned char shaOut[SHA256_DIGEST_LENGTH];
    sha256(pubComp, pubCompLen, shaOut);
    secureWipe(pubComp, sizeof(pubComp));

    unsigned char ripeOut[20];
    ripemd160(shaOut, SHA256_DIGEST_LENGTH, ripeOut);
    secureWipe(shaOut, sizeof(shaOut));

    std::vector<unsigned char> versioned;
    versioned.reserve(21);
    versioned.push_back(versionByte);
    versioned.insert(versioned.end(), ripeOut, ripeOut + 20);
    secureWipe(ripeOut, sizeof(ripeOut));

    w.address = base58check(versioned);
    secureWipe(versioned.data(), versioned.size());

    return w;
}

void BIP39::wipeWalletInfo(WalletInfo& w) {
    secureWipeString(w.mnemonic);
    secureWipeString(w.seedHex);
    secureWipeString(w.derivedPrivKey);
    secureWipeString(w.derivedPubKey);
    secureWipeString(w.address);
    secureWipeString(w.derivationPath);
    w.accountIndex = 0;
    w.addressIndex = 0;
}
