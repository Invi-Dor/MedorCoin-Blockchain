#include "crypto/verify_signature.h"
#include "crypto/secp256k1_wrapper.h"
#include "crypto/keccak256.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <functional>
#include <span>

namespace crypto {

using LogCallback = std::function<void(int, const char*, const char*)>;
static std::mutex    g_log_mutex;
static LogCallback   g_log_cb;

void setVerifySignatureLogger(LogCallback cb) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_cb = std::move(cb);
}

static bool logAndFail(int level, const char* fn, const char* reason) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_cb) g_log_cb(level, fn, reason);
    return false;
}

bool verifyHashWithPubkey(
    std::span<const unsigned char, 32> hash32,
    std::span<const unsigned char, 33> pubkey33,
    std::span<const unsigned char, 64> sig64)
{
    secp256k1_context* ctx = getCtx();

    secp256k1_pubkey pub;
    if (secp256k1_ec_pubkey_parse(
            ctx, &pub, pubkey33.data(), 33) != 1)
        return logAndFail(0, "verifyHashWithPubkey",
                          "failed to parse public key");

    secp256k1_ecdsa_signature sig;
    if (secp256k1_ecdsa_signature_parse_compact(
            ctx, &sig, sig64.data()) != 1)
        return logAndFail(0, "verifyHashWithPubkey",
                          "failed to parse compact signature");

    secp256k1_ecdsa_signature_normalize(ctx, &sig, &sig);

    if (secp256k1_ecdsa_verify(ctx, &sig, hash32.data(), &pub) != 1)
        return logAndFail(0, "verifyHashWithPubkey",
                          "signature verification failed");
    return true;
}

bool recoverPubkey(
    std::span<const unsigned char, 32> hash32,
    std::span<const unsigned char, 64> sig64,
    int                                recoveryId,
    std::span<unsigned char, 33>       pubkeyOut33)
{
    if (recoveryId < 0 || recoveryId > 3)
        return logAndFail(0, "recoverPubkey",
                          "recoveryId out of range 0-3");

    secp256k1_context* ctx = getCtx();

    secp256k1_ecdsa_recoverable_signature rsig;
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx, &rsig, sig64.data(), recoveryId) != 1)
        return logAndFail(0, "recoverPubkey",
                          "failed to parse recoverable signature");

    secp256k1_pubkey pub;
    if (secp256k1_ecdsa_recover(
            ctx, &pub, &rsig, hash32.data()) != 1)
        return logAndFail(0, "recoverPubkey",
                          "secp256k1_ecdsa_recover failed");

    size_t outLen = 33;
    if (secp256k1_ec_pubkey_serialize(
            ctx, pubkeyOut33.data(), &outLen,
            &pub, SECP256K1_EC_COMPRESSED) != 1)
        return logAndFail(0, "recoverPubkey",
                          "pubkey serialization failed");

    if (outLen != 33)
        return logAndFail(0, "recoverPubkey",
                          "serialized pubkey length is not 33");
    return true;
}

// =============================================================================
// recoverAndVerify
// Called by blockchain.cpp to verify a transaction signature and confirm
// the recovered address matches the expected UTXO owner address.
// =============================================================================
bool recoverAndVerify(
    const uint8_t*  hash32,
    const uint8_t*  rBytes,
    const uint8_t*  sBytes,
    int             v,
    const uint8_t*  expectedAddr20)
{
    if (!hash32 || !rBytes || !sBytes || !expectedAddr20)
        return false;

    int recoveryId = computeRecoveryId(
        static_cast<uint64_t>(v), 0);
    if (recoveryId < 0) {
        // Try EIP-155 with chainId 0
        if (v == 0 || v == 1) recoveryId = v;
        else return false;
    }

    unsigned char sig64[64];
    memcpy(sig64,      rBytes, 32);
    memcpy(sig64 + 32, sBytes, 32);

    unsigned char pubkey33[33];
    if (!recoverPubkey(
            std::span<const unsigned char, 32>(hash32, 32),
            std::span<const unsigned char, 64>(sig64,  64),
            recoveryId,
            std::span<unsigned char, 33>(pubkey33, 33)))
        return false;

    // Derive address: keccak256 of uncompressed pubkey[1..64], take last 20 bytes
    secp256k1_context* ctx = getCtx();
    secp256k1_pubkey pub;
    if (secp256k1_ec_pubkey_parse(ctx, &pub, pubkey33, 33) != 1)
        return false;

    unsigned char uncompressed[65];
    size_t uncompLen = 65;
    secp256k1_ec_pubkey_serialize(
        ctx, uncompressed, &uncompLen,
        &pub, SECP256K1_EC_UNCOMPRESSED);

    crypto::Keccak256Digest digest{};
    if (!crypto::Keccak256(uncompressed + 1, 64, digest))
        return false;

    // Address = last 20 bytes of keccak digest
    return memcmp(digest.data() + 12, expectedAddr20, 20) == 0;
}

int computeRecoveryId(uint64_t v, uint64_t chainId) {
    if (chainId > 0) {
        if (chainId <= (0xFFFFFFFFFFFFFFFFULL - 35) / 2) {
            uint64_t base = 35 + 2 * chainId;
            if (v == base || v == base + 1)
                return static_cast<int>(v - base);
        }
    }
    if (v == 27 || v == 28)
        return static_cast<int>(v - 27);
    return -1;
}

} // namespace crypto
