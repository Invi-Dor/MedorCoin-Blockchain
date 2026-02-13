#include "signature.h"
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <cstdio>

std::tuple<std::array<uint8_t,32>, std::array<uint8_t,32>, uint8_t>
signHash(const std::array<uint8_t,32> &digest,
         const std::string &privKeyPath) {

    FILE* fp = fopen(privKeyPath.c_str(), "r");
    if (!fp) throw std::runtime_error("Failed to open private key file");

    EVP_PKEY* pkey = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    fclose(fp);
    if (!pkey) throw std::runtime_error("Failed to read private key");

    EC_KEY* ec_key = EVP_PKEY_get1_EC_KEY(pkey);
    EVP_PKEY_free(pkey);

    ECDSA_SIG *sig = ECDSA_do_sign(digest.data(), digest.size(), ec_key);
    if (!sig) throw std::runtime_error("ECDSA sign failed");

    const BIGNUM *r_bn, *s_bn;
    ECDSA_SIG_get0(sig, &r_bn, &s_bn);

    std::array<uint8_t,32> r{}, s{};
    BN_bn2binpad(r_bn, r.data(), 32);
    BN_bn2binpad(s_bn, s.data(), 32);

    ECDSA_SIG_free(sig);
    EC_KEY_free(ec_key);

    uint8_t v = 27; // recovery id, adjust in full EVM logic

    return {r, s, v};
}
