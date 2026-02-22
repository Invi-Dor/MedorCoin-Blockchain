#include "bip39.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <secp256k1.h>
#include <iostream>
#include <fstream>
#include <sstream>

/* Base58 Alphabet */
static const char* BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/* Load BIP39 English wordlist */
std::vector<std::string> BIP39::loadWordList() {
    std::vector<std::string> list;
    std::ifstream file("bip39_wordlist.txt");
    std::string word;
    while (file >> word) list.push_back(word);
    return list;
}

/* Generate BIP39 mnemonic */
std::string BIP39::generateMnemonic() {
    auto words = loadWordList();
    if (words.size() != 2048) throw std::runtime_error("Missing BIP39 wordlist");

    unsigned char entropy[16]; // 128 bits
    if (!RAND_bytes(entropy, sizeof(entropy))) throw std::runtime_error("Entropy failure");

    std::string mnemonic;
    int index = 0;
    for (int i = 0; i < 12; i++) {
        index = entropy[i] % 2048;
        mnemonic += words[index];
        if (i < 11) mnemonic += " ";
    }
    return mnemonic;
}

/* PBKDF2 HMAC‑SHA512 seed derivation */
std::vector<unsigned char> BIP39::mnemonicToSeed(const std::string &mnemonic, const std::string &passphrase) {
    std::string salt = "mnemonic" + passphrase;
    std::vector<unsigned char> seed(64);
    PKCS5_PBKDF2_HMAC(
        mnemonic.c_str(), mnemonic.size(),
        reinterpret_cast<const unsigned char*>(salt.c_str()), salt.size(),
        2048, EVP_sha512(), 64, seed.data()
    );
    return seed;
}

/* Convert to hex */
std::string BIP39::toHex(const std::vector<unsigned char> &data) {
    std::ostringstream ss;
    for (auto byte : data) ss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    return ss.str();
}

/* Base58Check encoding */
static std::string encodeBase58(const std::vector<unsigned char> &input) {
    std::vector<unsigned long long> digits;
    digits.push_back(0);

    for (auto byte : input) {
        unsigned long long carry = byte;
        for (size_t j = 0; j < digits.size(); ++j) {
            carry += digits[j] << 8;
            digits[j] = carry % 58ULL;
            carry /= 58ULL;
        }
        while (carry) {
            digits.push_back(carry % 58ULL);
            carry /= 58ULL;
        }
    }

    std::string result;
    for (auto &c : input)
        if (c == 0) result += '1';

    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
        result += BASE58_ALPHABET[*it];
    return result;
}

/* Derive root keys and address */
BIP39::WalletInfo BIP39::deriveFromMnemonic(const std::string &mnemonic, const std::string &passphrase) {
    WalletInfo w;
    w.mnemonic = mnemonic;

    auto seed = mnemonicToSeed(mnemonic, passphrase);
    w.seedHex = toHex(seed);

    /* Create secp256k1 context */
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    unsigned char priv[32];
    memcpy(priv, seed.data(), 32); // seed first 32 bytes as private

    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, priv))
        throw std::runtime_error("Pubkey creation failed");

    unsigned char output[65];
    size_t outlen = 65;
    secp256k1_ec_pubkey_serialize(ctx, output, &outlen, &pubkey, SECP256K1_EC_UNCOMPRESSED);

    w.masterPrivKey = toHex({priv, priv + 32});
    w.masterPubKey = toHex({output, output + outlen});

    /* HASH160 */
    unsigned char sha[SHA256_DIGEST_LENGTH];
    SHA256(output, outlen, sha);
    unsigned char ripe[RIPEMD160_DIGEST_LENGTH];
    RIPEMD160(sha, SHA256_DIGEST_LENGTH, ripe);

    std::vector<unsigned char> versioned;
    versioned.push_back(0x00); // legacy prefix
    for (int i = 0; i < RIPEMD160_DIGEST_LENGTH; ++i) versioned.push_back(ripe[i]);

    std::vector<unsigned char> checksumInput = versioned;
    unsigned char first[SHA256_DIGEST_LENGTH], second[SHA256_DIGEST_LENGTH];
    SHA256(checksumInput.data(), checksumInput.size(), first);
    SHA256(first, SHA256_DIGEST_LENGTH, second);
    for (int i = 0; i < 4; i++) versioned.push_back(second[i]);

    w.address = encodeBase58(versioned);
    secp256k1_context_destroy(ctx);

    return w;
}
