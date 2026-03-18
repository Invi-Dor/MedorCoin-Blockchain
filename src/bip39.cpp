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
#include <climits>
#include <algorithm>
#include <cstdlib>

static_assert(CHAR_BIT == 8, "This code requires 8-bit bytes");

static constexpr size_t MAX_MNEMONIC_LEN   = 1024;
static constexpr size_t MAX_PASSPHRASE_LEN = 512;

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

template<size_t N>
struct SecureBuffer {
    unsigned char data[N];
    SecureBuffer()  { memset(data, 0, N); }
    ~SecureBuffer() { secureWipe(data, N); }
    SecureBuffer(const SecureBuffer&)            = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;
};

static secp256k1_context* getCtx() {
    static secp256k1_context* ctx = nullptr;
    static std::once_flag flag;
    std::call_once(flag, []() {
        ctx = secp256k1_context_create(
            SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
        if (!ctx)
            throw std::runtime_error("secp256k1_context_create failed");
    });
    return ctx;
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
            if (cache.size() == 2048) return;
            cache.clear();
        }
        throw std::runtime_error(
            "BIP39 wordlist not found. Set MEDOR_WORDLIST_PATH "
            "or place bip39_wordlist.txt in the working directory.");
    });
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
    unsigned char h1[SHA256_DIGEST_LENGTH], h2[SHA256_DIGEST_LENGTH];
    sha256(payload.data(), payload.size(), h1);
    sha256(h1, SHA256_DIGEST_LENGTH, h2);
    std::vector<unsigned char> full(payload);
    full.insert(full.end(), h2, h2 + 4);
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

struct MasterKey {
    SecureBuffer<32> privKey;
    SecureBuffer<32> chainCode;
};

static MasterKey deriveMaster(const std::vector<unsigned char>& seed) {
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
};

static ChildKey deriveChild(
    const unsigned char parentPriv[32],
    const unsigned char parentChain[32],
    unsigned int index)
{
    unsigned char data[37];
    unsigned char out[64];
    unsigned int  outLen = 64;
    bool hardened = (index >= 0x80000000u);
    if (hardened) {
        data[0] = 0x00;
        memcpy(data + 1, parentPriv, 32);
    } else {
        secp256k1_pubkey pub;
        if (secp256k1_ec_pubkey_create(getCtx(), &pub, parentPriv) != 1)
            throw std::runtime_error("Child key pubkey creation failed");
        size_t pubLen = 33;
        secp256k1_ec_pubkey_serialize(
            getCtx(), data, &pubLen, &pub, SECP256K1_EC_COMPRESSED);
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
    ChildKey ck;
    memcpy(ck.privKey.data,   out,      32);
    memcpy(ck.chainCode.data, out + 32, 32);
    secureWipe(out, sizeof(out));
    secureWipe(data, sizeof(data));
    if (secp256k1_ec_seckey_tweak_add(
            getCtx(), ck.privKey.data, parentPriv) != 1)
        throw std::runtime_error("Child key tweak_add failed.");
    if (secp256k1_ec_seckey_verify(getCtx(), ck.privKey.data) != 1)
        throw std::runtime_error("Child key outside curve range.");
    return ck;
}

static bool verifyChecksum(const std::string& mnemonic) {
    const auto& words = getWordList();
    std::vector<std::string> tokens;
    std::istringstream iss(mnemonic);
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);
    if (tokens.size() != 12) return false;
    unsigned char bits[17] = {};
    for (int i = 0; i < 12; i++) {
        auto it = std::find(words.begin(), words.end(), tokens[i]);
        if (it == words.end()) return false;
        unsigned int idx = static_cast<unsigned int>(it - words.begin());
        int bitOff  = i * 11;
        int byteIdx = bitOff / 8;
        int bitPos  = bitOff % 8;
        unsigned int shifted = idx << (13 - bitPos);
        bits[byteIdx]     |= static_cast<unsigned char>((shifted >> 16) & 0xFF);
        bits[byteIdx + 1] |= static_cast<unsigned char>((shifted >>  8) & 0xFF);
        if (byteIdx + 2 < 17)
            bits[byteIdx + 2] |= static_cast<unsigned char>(shifted & 0xFF);
    }
    unsigned char hash[SHA256_DIGEST_LENGTH];
    sha256(bits, 16, hash);
    bool valid = ((hash[0] & 0xF0) == (bits[16] & 0xF0));
    secureWipe(bits, sizeof(bits));
    secureWipe(hash, sizeof(hash));
    return valid;
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
        secureWipe(const_cast<char*>(salt.data()), salt.size());
        throw std::runtime_error("PBKDF2-HMAC-SHA512 failed");
    }
    secureWipe(const_cast<char*>(salt.data()), salt.size());
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
    const std::string& passphrase)
{
    checkLength(mnemonic,   MAX_MNEMONIC_LEN,   "mnemonic");
    checkLength(passphrase, MAX_PASSPHRASE_LEN, "passphrase");

    WalletInfo w;
    w.mnemonic = mnemonic;

    auto seed = mnemonicToSeed(mnemonic, passphrase);
    w.seedHex = toHex(seed);

    MasterKey master = deriveMaster(seed);
    secureWipe(seed.data(), seed.size());

    ChildKey lvl1 = deriveChild(
        master.privKey.data, master.chainCode.data, 0x8000002Cu);
    ChildKey lvl2 = deriveChild(
        lvl1.privKey.data, lvl1.chainCode.data, 0x80000000u);
    ChildKey lvl3 = deriveChild(
        lvl2.privKey.data, lvl2.chainCode.data, 0x80000000u);
    ChildKey lvl4 = deriveChild(
        lvl3.privKey.data, lvl3.chainCode.data, 0u);
    ChildKey lvl5 = deriveChild(
        lvl4.privKey.data, lvl4.chainCode.data, 0u);

    secp256k1_pubkey pubkey;
    if (secp256k1_ec_pubkey_create(
            getCtx(), &pubkey, lvl5.privKey.data) != 1)
        throw std::runtime_error("secp256k1 pubkey creation failed");

    unsigned char pubComp[33];
    size_t pubCompLen = 33;
    secp256k1_ec_pubkey_serialize(
        getCtx(), pubComp, &pubCompLen, &pubkey, SECP256K1_EC_COMPRESSED);

    w.masterPrivKey = toHex({lvl5.privKey.data, lvl5.privKey.data + 32});
    w.masterPubKey  = toHex({pubComp, pubComp + pubCompLen});

    unsigned char shaOut[SHA256_DIGEST_LENGTH];
    sha256(pubComp, pubCompLen, shaOut);
    secureWipe(pubComp, sizeof(pubComp));

    unsigned char ripeOut[20];
    ripemd160(shaOut, SHA256_DIGEST_LENGTH, ripeOut);
    secureWipe(shaOut, sizeof(shaOut));

    std::vector<unsigned char> versioned;
    versioned.reserve(21);
    versioned.push_back(0x00);
    versioned.insert(versioned.end(), ripeOut, ripeOut + 20);
    secureWipe(ripeOut, sizeof(ripeOut));

    w.address = base58check(versioned);
    secureWipe(versioned.data(), versioned.size());

    return w;
}
