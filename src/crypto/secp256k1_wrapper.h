// File: crypto/secp256k1_wrapper.h
// SPDX‑License‑Identifier: MIT
#ifndef CRYPTO_SECP256K1_WRAPPER_H
#define CRYPTO_SECP256K1_WRAPPER_H

#include <array>
#include <vector>
#include <optional>

namespace crypto {

struct Secp256k1Signature {
    std::array<uint8_t, 32> r;
    std::array<uint8_t, 32> s;
    int recid;
};

struct Secp256k1Keypair {
    std::array<uint8_t, 32> privkey;
    std::array<uint8_t, 65> pubkey_uncompressed;
};

// Generate keypair using existing libsecp256k1 context
Secp256k1Keypair generateKeypair();

// Sign a 32‑byte hash using secp256k1 recoverable signature
std::optional<Secp256k1Signature> signRecoverable(
    const uint8_t hash[32],
    const std::array<uint8_t,32>& privkey
);

// Recover public key from signature+hash
std::optional<std::array<uint8_t,65>> recoverPubkey(
    const uint8_t hash[32],
    const Secp256k1Signature& sig
);

} // namespace crypto

#endif // CRYPTO_SECP256K1_WRAPPER_H
