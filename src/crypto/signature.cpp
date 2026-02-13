#include "signature.h"
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/pem.h>
#include <openssl/bn.h>
#include <stdexcept>
#include <cstdio>

// Sign a 32‑byte Keccak digest with a secp256k1 private key.
// Returns tuple (r, s, v) where:
// - r and s are the signature components,
// - v is the recovery id (calculated later externally).
std::tuple<std::array<uint8_t,32>, std::array<uint8_t,32>, uint8_t>
signHash(const std::array<uint8_t,32> &digest,
         const std::string &privKeyPath) {

    // Open private key file
    FILE *fp = fopen(privKeyPath.c_str(), "r");
    if (!fp) {
        throw std::runtime_error("Failed to open private key file");
    }

    // Load private key
    EVP_PKEY *pkey = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    fclose(fp);
    if (!pkey) {
        throw std::runtime_error("Failed to read private key PEM");
    }

    // Extract EC key (secp256k1)
    EC_KEY *ec_key = EVP_PKEY_get1_EC_KEY(pkey);
    EVP_PKEY_free(pkey);
    if (!ec_key) {
        throw std::runtime_error("Invalid EC private key");
    }

    // Sign the 32‑byte hash
    ECDSA_SIG *sig = ECDSA_do_sign(digest.data(), digest.size(), ec_key);
    if (!sig) {
        EC_KEY_free(ec_key);
        throw std::runtime_error("ECDSA signing failed");
    }

    // Get r and s BIGNUMs from signature
    const BIGNUM *r_bn = nullptr;
    const BIGNUM *s_bn = nullptr;
    ECDSA_SIG_get0(sig, &r_bn, &s_bn);

    std::array<uint8_t, 32> r_bytes = {};
    std::array<uint8_t, 32> s_bytes = {};
    BN_bn2binpad(r_bn, r_bytes.data(), 32);
    BN_bn2binpad(s_bn, s_bytes.data(), 32);

    // Cleanup
    ECDSA_SIG_free(sig);
    EC_KEY_free(ec_key);

    // Recovery ID will be computed separately
    uint8_t v = 0;
    return {r_bytes, s_bytes, v};
}
