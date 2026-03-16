#include "crypto/secp256k1_wrapper.h"

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <cstring>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// Platform-portable CSPRNG
// ─────────────────────────────────────────────────────────────────────────────

#if defined(_WIN32) || defined(_WIN64)
#  define MEDOR_PLATFORM_WINDOWS 1
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#else
#  define MEDOR_PLATFORM_POSIX 1
#  include <fstream>
#endif

namespace crypto {

static bool secureRandom(uint8_t *buf, size_t n) noexcept
{
#if defined(MEDOR_PLATFORM_WINDOWS)
    // BCryptGenRandom is the Windows CSPRNG recommended by Microsoft.
    NTSTATUS status = BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(buf),
        static_cast<ULONG>(n),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        std::cerr << "[secp256k1] secureRandom: BCryptGenRandom failed "
                     "with status 0x" << std::hex << status << "\n";
        return false;
    }
    return true;
#else
    // /dev/urandom is non-blocking and suitable for key material on Linux/macOS.
    std::ifstream f("/dev/urandom", std::ios::binary);
    if (!f) {
        std::cerr << "[secp256k1] secureRandom: cannot open /dev/urandom\n";
        return false;
    }
    f.read(reinterpret_cast<char *>(buf), static_cast<std::streamsize>(n));
    if (!f.good()) {
        std::cerr << "[secp256k1] secureRandom: short read from /dev/urandom\n";
        return false;
    }
    return true;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared context — created once, never destroyed
// ─────────────────────────────────────────────────────────────────────────────

secp256k1_context *getCtx() noexcept
{
    static secp256k1_context *ctx = []() noexcept -> secp256k1_context * {
        secp256k1_context *c = secp256k1_context_create(
            SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
        if (!c) {
            std::cerr << "[secp256k1] FATAL: context creation failed — "
                         "terminating\n";
            std::terminate();
        }

        // Randomise the context to protect against side-channel attacks
        // (recommended by libsecp256k1 documentation)
        uint8_t seed[32] = {};
        if (secureRandom(seed, 32)) {
            secp256k1_context_randomize(c, seed);
            std::memset(seed, 0, sizeof(seed));
        } else {
            std::cerr << "[secp256k1] WARNING: context randomisation skipped "
                         "due to RNG failure\n";
        }
        return c;
    }();
    return ctx;
}

// ─────────────────────────────────────────────────────────────────────────────
// Generate keypair
// ─────────────────────────────────────────────────────────────────────────────

std::optional<Secp256k1Keypair> generateKeypair() noexcept
{
    secp256k1_context *ctx = getCtx();
    Secp256k1Keypair   kp;

    for (int attempt = 0; attempt < 32; ++attempt) {
        if (!secureRandom(kp.privkey.data(), 32)) return std::nullopt;

        // Verify the key is in the valid secp256k1 range [1, n-1]
        if (!secp256k1_ec_seckey_verify(ctx, kp.privkey.data())) continue;

        secp256k1_pubkey pub;
        if (!secp256k1_ec_pubkey_create(ctx, &pub, kp.privkey.data()))
            continue;

        size_t outLen = 65;
        secp256k1_ec_pubkey_serialize(
            ctx, kp.pubkey_uncompressed.data(), &outLen,
            &pub, SECP256K1_EC_UNCOMPRESSED);

        if (outLen != 65) {
            std::cerr << "[secp256k1] generateKeypair: unexpected pubkey "
                         "serialization length\n";
            return std::nullopt;
        }
        return kp;
    }

    std::cerr << "[secp256k1] generateKeypair: failed to generate valid "
                 "key after 32 attempts\n";
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sign (recoverable)
// ─────────────────────────────────────────────────────────────────────────────

std::optional<Secp256k1Signature> signRecoverable(
    const uint8_t               hash32[32],
    const std::array<uint8_t, 32> &privkey) noexcept
{
    if (!hash32) {
        std::cerr << "[secp256k1] signRecoverable: null hash pointer\n";
        return std::nullopt;
    }

    secp256k1_context *ctx = getCtx();

    if (!secp256k1_ec_seckey_verify(ctx, privkey.data())) {
        std::cerr << "[secp256k1] signRecoverable: invalid private key\n";
        return std::nullopt;
    }

    secp256k1_ecdsa_recoverable_signature sigRec;
    if (!secp256k1_ecdsa_sign_recoverable(
            ctx, &sigRec, hash32, privkey.data(), nullptr, nullptr)) {
        std::cerr << "[secp256k1] signRecoverable: signing operation failed\n";
        return std::nullopt;
    }

    uint8_t compact[64] = {};
    int     recid       = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        ctx, compact, &recid, &sigRec);

    if (recid < 0 || recid > 3) {
        std::cerr << "[secp256k1] signRecoverable: unexpected recid="
                  << recid << "\n";
        return std::nullopt;
    }

    Secp256k1Signature out;
    std::memcpy(out.r.data(), compact,      32);
    std::memcpy(out.s.data(), compact + 32, 32);
    out.recid = recid;
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Recover public key
// ─────────────────────────────────────────────────────────────────────────────

std::optional<std::array<uint8_t, 65>> recoverPubkey(
    const uint8_t             hash32[32],
    const Secp256k1Signature &sig) noexcept
{
    if (!hash32) {
        std::cerr << "[secp256k1] recoverPubkey: null hash pointer\n";
        return std::nullopt;
    }
    if (sig.recid < 0 || sig.recid > 3) {
        std::cerr << "[secp256k1] recoverPubkey: invalid recid="
                  << sig.recid << "\n";
        return std::nullopt;
    }

    secp256k1_context *ctx = getCtx();

    uint8_t compact[64] = {};
    std::memcpy(compact,      sig.r.data(), 32);
    std::memcpy(compact + 32, sig.s.data(), 32);

    secp256k1_ecdsa_recoverable_signature sigRec;
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx, &sigRec, compact, sig.recid)) {
        std​​​​​​​​​​​​​​​​
