#include "crypto/secp256k1_wrapper.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <mutex>
#include <atomic>
#include <span>

#if defined(_WIN32) || defined(_WIN64)
#  define MEDOR_WINDOWS 1
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#else
#  include <fstream>
#  include <sys/random.h>
#endif

namespace crypto {

// =============================================================================
// STRUCTURED LOGGER
// =============================================================================
static std::mutex          g_log_mutex;
static WrapperLogCallback  g_log_cb;

void setWrapperLogger(WrapperLogCallback cb) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_cb = std::move(cb);
}

static void wlog(int level, const char* fn, const char* msg) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_cb) g_log_cb(level, fn, msg);
}

// =============================================================================
// SECURE RANDOM
// Priority order:
//   1. getrandom() syscall (Linux 3.17+, glibc 2.25+) — most reliable
//   2. /dev/urandom — standard POSIX fallback
//   3. BCryptGenRandom on Windows
// All failures are logged with a reason. Returns false only if all sources
// fail, which should be treated as a fatal node startup error.
// =============================================================================
static bool secureRandom(uint8_t* buf, size_t n) noexcept {
    if (!buf || n == 0) {
        wlog(0, "secureRandom", "null buffer or zero length");
        return false;
    }

#if defined(MEDOR_WINDOWS)
    NTSTATUS status = BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(buf),
        static_cast<ULONG>(n),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        char msg[64];
        std::snprintf(msg, sizeof(msg),
            "BCryptGenRandom failed with NTSTATUS 0x%08lX",
            static_cast<unsigned long>(status));
        wlog(0, "secureRandom", msg);
        return false;
    }
    return true;

#else
    // Attempt 1: getrandom() syscall — non-blocking, no fd required
#  if defined(__linux__) && defined(SYS_getrandom)
    {
        ssize_t ret = ::getrandom(buf, n, 0);
        if (ret == static_cast<ssize_t>(n)) return true;
        wlog(1, "secureRandom",
             "getrandom() failed or returned short read, falling back");
    }
#  endif

    // Attempt 2: /dev/urandom
    {
        std::ifstream f("/dev/urandom", std::ios::binary);
        if (f.is_open()) {
            f.read(reinterpret_cast<char*>(buf),
                   static_cast<std::streamsize>(n));
            if (f.good() && static_cast<size_t>(f.gcount()) == n)
                return true;
            wlog(1, "secureRandom",
                 "/dev/urandom read failed or returned short data");
        } else {
            wlog(1, "secureRandom",
                 "/dev/urandom not available on this system");
        }
    }

    wlog(0, "secureRandom",
         "all entropy sources exhausted — node cannot generate keys safely");
    return false;
#endif
}

// =============================================================================
// ENTROPY HEALTH CHECK
// Called once during context initialization. Verifies the RNG produces
// non-constant, non-zero output. This is a basic sanity check — it does
// not replace OS-level entropy auditing.
// =============================================================================
static bool entropyHealthCheck() noexcept {
    uint8_t a[32], b[32];
    if (!secureRandom(a, 32) || !secureRandom(b, 32)) {
        wlog(0, "entropyHealthCheck", "RNG produced no output");
        return false;
    }
    if (memcmp(a, b, 32) == 0) {
        wlog(0, "entropyHealthCheck",
             "RNG produced identical outputs — entropy source broken");
        return false;
    }
    bool allZero = true;
    for (int i = 0; i < 32; i++)
        if (a[i] != 0) { allZero = false; break; }
    if (allZero) {
        wlog(0, "entropyHealthCheck",
             "RNG produced all-zero output — entropy source broken");
        return false;
    }
    // Wipe test bytes
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    return true;
}

// =============================================================================
// CONTEXT
// Created once. On failure throws std::runtime_error so the node can
// handle it gracefully rather than calling std::terminate.
// Context is randomized at creation for side-channel resistance per
// libsecp256k1 documentation.
// Registered for cleanup at exit.
// =============================================================================
static secp256k1_context* g_ctx = nullptr;

static void cleanupCtx() {
    if (g_ctx) {
        secp256k1_context_destroy(g_ctx);
        g_ctx = nullptr;
    }
}

secp256k1_context* getCtx() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        if (!entropyHealthCheck())
            throw std::runtime_error(
                "[secp256k1] entropy health check failed at context init. "
                "Node cannot operate safely without a working RNG.");

        g_ctx = secp256k1_context_create(
            SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
        if (!g_ctx)
            throw std::runtime_error(
                "[secp256k1] secp256k1_context_create returned null. "
                "This should never happen on a correctly built system.");

        uint8_t seed[32] = {};
        if (secureRandom(seed, 32)) {
            secp256k1_context_randomize(g_ctx, seed);
            memset(seed, 0, sizeof(seed));
        } else {
            wlog(1, "getCtx",
                 "context randomization skipped due to RNG failure. "
                 "Side-channel protection is reduced.");
        }

        std::atexit(cleanupCtx);
        wlog(2, "getCtx", "secp256k1 context initialized successfully");
    });
    return g_ctx;
}

// =============================================================================
// generateKeypair
// Retries up to 64 times with detailed logging per attempt failure.
// Returns both compressed (33 bytes) and uncompressed (65 bytes) public keys
// so callers can use whichever format they require without a second call.
// Private key is validated against the curve order on every attempt.
// =============================================================================
std::optional<Secp256k1Keypair> generateKeypair() noexcept {
    secp256k1_context* ctx = nullptr;
    try {
        ctx = getCtx();
    } catch (const std::exception& e) {
        wlog(0, "generateKeypair", e.what());
        return std::nullopt;
    }

    static constexpr int MAX_ATTEMPTS = 64;
    Secp256k1Keypair kp;

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        if (!secureRandom(kp.privkey.data(), 32)) {
            wlog(0, "generateKeypair", "RNG failure during key generation");
            return std::nullopt;
        }

        if (secp256k1_ec_seckey_verify(ctx, kp.privkey.data()) != 1) {
            wlog(2, "generateKeypair",
                 "generated key outside curve range, retrying");
            continue;
        }

        secp256k1_pubkey pub;
        if (secp256k1_ec_pubkey_create(ctx, &pub, kp.privkey.data()) != 1) {
            wlog(1, "generateKeypair",
                 "pubkey creation failed, retrying");
            continue;
        }

        // Serialize compressed
        size_t compLen = 33;
        if (secp256k1_ec_pubkey_serialize(
                ctx, kp.pubkey_compressed.data(), &compLen,
                &pub, SECP256K1_EC_COMPRESSED) != 1 || compLen != 33) {
            wlog(1, "generateKeypair",
                 "compressed pubkey serialization failed, retrying");
            continue;
        }

        // Serialize uncompressed
        size_t uncompLen = 65;
        if (secp256k1_ec_pubkey_serialize(
                ctx, kp.pubkey_uncompressed.data(), &uncompLen,
                &pub, SECP256K1_EC_UNCOMPRESSED) != 1 || uncompLen != 65) {
            wlog(1, "generateKeypair",
                 "uncompressed pubkey serialization failed, retrying");
            continue;
        }

        wlog(2, "generateKeypair", "keypair generated successfully");
        return kp;
    }

    wlog(0, "generateKeypair",
         "failed to generate valid keypair after 64 attempts. "
         "RNG may be broken.");
    return std::nullopt;
}

// =============================================================================
// signRecoverable
// Buffer sizes enforced at compile time via std::span extents.
// Validates private key before signing.
// recid is always 0 or 1 for standard secp256k1 signing.
// =============================================================================
std::optional<Secp256k1Signature> signRecoverable(
    std::span<const uint8_t, 32> hash32,
    std::span<const uint8_t, 32> privkey) noexcept
{
    secp256k1_context* ctx = nullptr;
    try {
        ctx = getCtx();
    } catch (const std::exception& e) {
        wlog(0, "signRecoverable", e.what());
        return std::nullopt;
    }

    if (secp256k1_ec_seckey_verify(ctx, privkey.data()) != 1) {
        wlog(0, "signRecoverable",
             "private key failed curve validation");
        return std::nullopt;
    }

    secp256k1_ecdsa_recoverable_signature sigRec;
    if (secp256k1_ecdsa_sign_recoverable(
            ctx, &sigRec,
            hash32.data(), privkey.data(),
            nullptr, nullptr) != 1) {
        wlog(0, "signRecoverable", "signing operation failed");
        return std::nullopt;
    }

    uint8_t compact[64] = {};
    int     recid       = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        ctx, compact, &recid, &sigRec);

    if (recid < 0 || recid > 1) {
        wlog(0, "signRecoverable",
             "unexpected recid outside 0-1 from standard signing");
        return std::nullopt;
    }

    Secp256k1Signature out;
    memcpy(out.r.data(), compact,      32);
    memcpy(out.s.data(), compact + 32, 32);
    out.recid = recid;
    return out;
}

// =============================================================================
// INTERNAL: shared pubkey recovery logic
// =============================================================================
static std::optional<secp256k1_pubkey> recoverRawPubkey(
    std::span<const uint8_t, 32> hash32,
    const Secp256k1Signature&    sig) noexcept
{
    if (sig.recid < 0 || sig.recid > 3) {
        wlog(0, "recoverRawPubkey", "recoveryId out of range 0-3");
        return std::nullopt;
    }

    secp256k1_context* ctx = nullptr;
    try {
        ctx = getCtx();
    } catch (const std::exception& e) {
        wlog(0, "recoverRawPubkey", e.what());
        return std::nullopt;
    }

    uint8_t compact[64] = {};
    memcpy(compact,      sig.r.data(), 32);
    memcpy(compact + 32, sig.s.data(), 32);

    secp256k1_ecdsa_recoverable_signature sigRec;
    if (secp256k1_ecdsa_recoverable_signature_parse_compact(
            ctx, &sigRec, compact, sig.recid) != 1) {
        wlog(0, "recoverRawPubkey",
             "failed to parse recoverable signature");
        return std::nullopt;
    }

    secp256k1_pubkey pub;
    if (secp256k1_ecdsa_recover(ctx, &pub, &sigRec, hash32.data()) != 1) {
        wlog(0, "recoverRawPubkey", "secp256k1_ecdsa_recover failed");
        return std::nullopt;
    }

    return pub;
}

// =============================================================================
// recoverPubkeyUncompressed
// Returns 65-byte uncompressed public key for legacy compatibility.
// =============================================================================
std::optional<std::array<uint8_t, 65>> recoverPubkeyUncompressed(
    std::span<const uint8_t, 32> hash32,
    const Secp256k1Signature&    sig) noexcept
{
    auto pubOpt = recoverRawPubkey(hash32, sig);
    if (!pubOpt) return std::nullopt;

    secp256k1_context* ctx = getCtx();
    std::array<uint8_t, 65> out{};
    size_t outLen = 65;

    if (secp256k1_ec_pubkey_serialize(
            ctx, out.data(), &outLen,
            &pubOpt.value(),
            SECP256K1_EC_UNCOMPRESSED) != 1 || outLen != 65) {
        wlog(0, "recoverPubkeyUncompressed",
             "uncompressed serialization failed or wrong length");
        return std::nullopt;
    }
    return out;
}

// =============================================================================
// recoverPubkeyCompressed
// Returns 33-byte compressed public key for modern use.
// =============================================================================
std::optional<std::array<uint8_t, 33>> recoverPubkeyCompressed(
    std::span<const uint8_t, 32> hash32,
    const Secp256k1Signature&    sig) noexcept
{
    auto pubOpt = recoverRawPubkey(hash32, sig);
    if (!pubOpt) return std::nullopt;

    secp256k1_context* ctx = getCtx();
    std::array<uint8_t, 33> out{};
    size_t outLen = 33;

    if (secp256k1_ec_pubkey_serialize(
            ctx, out.data(), &outLen,
            &pubOpt.value(),
            SECP256K1_EC_COMPRESSED) != 1 || outLen != 33) {
        wlog(0, "recoverPubkeyCompressed",
             "compressed serialization failed or wrong length");
        return std::nullopt;
    }
    return out;
}

} // namespace crypto
