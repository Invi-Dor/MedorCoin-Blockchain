#include "poa_sign.h"
#include "crypto/signature.h"
#include "crypto/signature_helpers.h"
#include "consensus/validator_registry.h"
#include <evmc/evmc.hpp>
#include <iostream>

// Helper to convert hex‑string address into byte array
static std::array<uint8_t,20> hexToAddress(const std::string &hex) {
    std::array<uint8_t,20> addr{};
    for (size_t i = 0; i < 20; ++i) {
        std::string byteHex = hex.substr(i * 2, 2);
        addr[i] = static_cast<uint8_t>(std::stoul(byteHex, nullptr, 16));
    }
    return addr;
}

// Sign a block header using the validator’s key
std::string signBlockPoA(const Block &block, const std::string &validatorAddr) {
    // 1) Serialize header bytes
    std::vector<uint8_t> headerBytes = block.serializeHeader();

    // 2) Hash
    uint8_t hashBytes[32];
    keccak(headerBytes.data(), headerBytes.size(), hashBytes, 32);
    std::vector<uint8_t> hashVec(hashBytes, hashBytes + 32);

    // 3) Lookup private key (stub — replace with secure store)
    std::string privKeyHex = ValidatorRegistry::getPrivateKey(validatorAddr);
    std::vector<uint8_t> privKey(32);
    for (size_t i = 0; i < 32; ++i)
        privKey[i] = std::stoul(privKeyHex.substr(i*2, 2), nullptr, 16);

    // 4) ECDSA recoverable sign
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    secp256k1_ecdsa_recoverable_signature sigRec;
    secp256k1_ecdsa_sign_recoverable(ctx, &sigRec,
                                     hashVec.data(),
                                     privKey.data(),
                                     nullptr, nullptr);

    unsigned char output[64];
    int recid;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx,
                                                           output,
                                                           &recid,
                                                           &sigRec);
    secp256k1_context_destroy(ctx);

    std::ostringstream ss;
    for (int i = 0; i < 64; i++)
        ss << std::hex << std::setw(2) << std::setfill('0')
           << (int)output[i];

    // Include recovery id
    ss << std::hex << recid;

    return ss.str();
}

// Verify a PoA block signature
bool verifyBlockPoA(const Block &block) {
    if (block.signature.empty()) return false;

    // 1) Extract signature bytes
    std::string sigHex = block.signature;
    std::vector<uint8_t> sigBytes(64);
    for (size_t i = 0; i < 64; ++i)
        sigBytes[i] = std::stoul(sigHex.substr(i*2,2), nullptr, 16);

    int recid = std::stoi(sigHex.substr(128,1), nullptr, 16);

    // 2) Hash block header
    std::vector<uint8_t> hdrBytes = block.serializeHeader();
    uint8_t hash[32];
    keccak(hdrBytes.data(), hdrBytes.size(), hash, 32);

    // 3) Recover public key
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    secp256k1_ecdsa_recoverable_signature sigRec;
    secp256k1_ecdsa_recoverable_signature_parse_compact(ctx,
                                                       &sigRec,
                                                       sigBytes.data(),
                                                       recid);

    secp256k1_pubkey pubkey;
    if (!secp256k1_ecdsa_recover(ctx, &pubkey, &sigRec, hash)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // 4) Serialize public key
    unsigned char pubBytes[65];
    size_t outLen = 65;
    secp256k1_ec_pubkey_serialize(ctx,
                                  pubBytes,
                                  &outLen,
                                  &pubkey,
                                  SECP256K1_EC_UNCOMPRESSED);
    secp256k1_context_destroy(ctx);

    // 5) Derive address (last 20 bytes of keccak of pubkey)
    uint8_t addrHash[32];
    keccak(pubBytes+1, 64, addrHash, 32);

    std::array<uint8_t,20> addr;
    std::copy(addrHash+12, addrHash+32, addr.begin());

    // 6) Check if derived address is in validator set
    return ValidatorRegistry::isValidator(addr);
}
