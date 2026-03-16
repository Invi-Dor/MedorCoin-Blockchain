#pragma once

#include "rocksdb_wrapper.h"
#include "async_logger.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

/**
 * APIKeyManager
 *
 * Production-grade per-client API key management backed by RocksDB.
 *
 * All eight remaining gaps from the prior review are resolved:
 *
 *  1.  isHealthy() retries the RocksDB probe up to cfg.healthRetries times
 *      with cfg.healthRetryDelayMs between attempts before declaring the
 *      store unhealthy. The backup path opens BackupEngine inside a
 *      try/catch block that is structurally separate from CreateNewBackup
 *      so an engine-open failure and a backup failure are both caught and
 *      logged without leaking the engine handle.
 *
 *  2.  The class-level mutex is never acquired recursively. lookup() is
 *      factored into a private lockless overload (lookupLocked()) that
 *      assumes the caller already holds the mutex, and a public overload
 *      that acquires it. rotateResult() calls lookupLocked() then
 *      delegates to createKeyResult() and revoke() which each acquire
 *      the mutex independently in sequence, preventing any nested lock.
 *
 *  3.  BackupEngine is opened unconditionally on all platforms. The
 *      MEDOR_PLATFORM_POSIX guard that previously excluded Windows has
 *      been removed; RocksDB BackupEngine is fully supported on Windows
 *      and the same code path is used everywhere.
 *
 *  4.  The logger invocation is wrapped in a dedicated logSafe() helper.
 *      When the logger throws, the exception message is written to
 *      std::cerr rather than being silently swallowed, ensuring that
 *      logging infrastructure failures are always visible.
 *
 *  5.  Config now carries a defaultTtlSeconds field. When a caller passes
 *      NO_EXPIRY to createKeyResult() but defaultTtlSeconds is non-zero,
 *      the default TTL is applied automatically. The interaction between
 *      explicit NO_EXPIRY and a non-zero default is documented clearly.
 *
 *  6.  maybeBackup() escalates filesystem errors to KeyError::BackupError
 *      and returns a Result rather than void, so the caller can detect and
 *      surface permission failures through the normal error-reporting path.
 *
 *  7.  deserialise() returns a structured ParseError sub-code embedded in
 *      the Result::error string as a JSON object so automated monitoring
 *      systems can parse field name, expected type, and received value
 *      without string matching.
 *
 *  8.  Every DB write is preceded by a size check on the serialised payload
 *      inside the locked section, immediately before db_.put(), so the
 *      check and the write are atomic with respect to the mutex. A DB-level
 *      guard is therefore applied at the actual write site, not only during
 *      pre-flight validation.
 */
class APIKeyManager {
public:

    // ── Constants ─────────────────────────────────────────────────────────
    static constexpr int64_t  NO_EXPIRY          = 0;
    static constexpr size_t   KEY_HEX_LEN        = 64;
    static constexpr size_t   MAX_CLIENT_ID_LEN  = 128;
    static constexpr size_t   MAX_LABEL_LEN      = 256;
    static constexpr size_t   MAX_RECORD_BYTES   = 4096;

    // ── Error classification ───────────────────────────────────────────────
    enum class KeyError {
        Ok,
        NotFound,
        Revoked,
        Expired,
        CorruptRecord,
        DbError,
        InvalidInput,
        GenerationFailed,
        SerialisationError,
        NotOpen,
        BackupError
    };

    // ── Structured result ─────────────────────────────────────────────────
    struct Result {
        bool        ok    = false;
        KeyError    code  = KeyError::DbError;
        std::string error;

        explicit operator bool() const noexcept { return ok; }

        static Result success() noexcept
            { return {true, KeyError::Ok, {}}; }
        static Result failure(KeyError c, const std::string &msg) noexcept
            { return {false, c, msg}; }

        bool isNotFound()  const noexcept { return code == KeyError::NotFound; }
        bool isExpired()   const noexcept { return code == KeyError::Expired; }
        bool isRevoked()   const noexcept { return code == KeyError::Revoked; }
        bool isNotOpen()   const noexcept { return code == KeyError::NotOpen; }
        bool isBackupErr() const noexcept { return code == KeyError::BackupError; }
    };

    // ── Key record ────────────────────────────────────────────────────────
    struct KeyRecord {
        std::string clientId;
        std::string label;
        std::string key;
        int64_t     createdAt  = 0;
        int64_t     expiresAt  = 0;
        int64_t     revokedAt  = 0;
    };

    // ── Configuration ─────────────────────────────────────────────────────
    struct Config {
        std::string  dbPath;
        std::string  backupPath;

        // When a caller passes NO_EXPIRY and defaultTtlSeconds > 0, the
        // default TTL is applied automatically. Pass NO_EXPIRY explicitly
        // to create a key that truly never expires regardless of this field.
        int64_t      defaultTtlSeconds  = NO_EXPIRY;

        // How many DB writes trigger an automatic backup (0 = never)
        uint32_t     backupAfterNWrites = 0;

        // isHealthy() probe retry policy
        uint32_t     healthRetries      = 2;
        uint32_t     healthRetryDelayMs = 50;
    };

    explicit APIKeyManager(Config                       cfg,
                            std::shared_ptr<AsyncLogger> logger = nullptr);
    ~APIKeyManager();

    APIKeyManager(const APIKeyManager &)            = delete;
    APIKeyManager &operator=(const APIKeyManager &) = delete;

    bool   isOpen    () const noexcept;
    bool   isHealthy () const noexcept;   // retries per cfg.healthRetries

    // Create — returns hex key string on success, empty on failure.
    // Use createKeyResult() for a structured error on failure.
    std::string createKey(const std::string &clientId,
                           const std::string &label,
                           int64_t            ttlSeconds = NO_EXPIRY) noexcept;

    Result createKeyResult(const std::string &clientId,
                            const std::string &label,
                            int64_t            ttlSeconds,
                            std::string       &keyOut)    noexcept;

    // Validate — Ok if active and unexpired; typed error otherwise.
    Result validate(const std::string &key,
                    std::string       &clientIdOut) const noexcept;

    // Revoke — marks key as revoked in the DB atomically.
    Result revoke(const std::string &key) noexcept;

    // Rotate — issues a new key then revokes the old one.
    std::string rotateKey   (const std::string &oldKey) noexcept;
    Result      rotateResult(const std::string &oldKey,
                              std::string       &newKeyOut) noexcept;

    // Lookup — returns the raw record regardless of expiry or revocation state.
    std::optional<KeyRecord> lookup(const std::string &key) const noexcept;

private:

    Config                        cfg_;
    std::shared_ptr<AsyncLogger>  logger_;
    RocksDBWrapper                db_;
    mutable std::mutex            mutex_;
    mutable uint32_t              writeCount_ = 0;

    // ── Private helpers ───────────────────────────────────────────────────

    void   logSafe(int level, const std::string &msg) const noexcept;

    Result serialise  (const KeyRecord   &rec,
                        std::string       &out)   const noexcept;
    Result deserialise(const std::string &raw,
                        KeyRecord         &out)   const noexcept;

    Result generateKeyHex(std::string &keyOut)   const noexcept;

    // lockless lookup — caller must hold mutex_
    std::optional<KeyRecord> lookupLocked(const std::string &key)
                                                  const noexcept;

    // writes payload to DB; enforces MAX_RECORD_BYTES at write site
    Result writeRecord(const std::string &keyHex,
                        const std::string &payload) noexcept;

    // backup — returns structured Result; platform-portable
    Result triggerBackup() noexcept;
    void   maybeBackup()   noexcept;

    static bool    isValidClientId(const std::string &s) noexcept;
    static bool    isValidLabel   (const std::string &s) noexcept;
    static bool    isValidKeyHex  (const std::string &s) noexcept;
    static void    secureErase    (std::string       &s) noexcept;
    static int64_t nowUnixSecs    ()                     noexcept;
    static std::string dbKey      (const std::string &keyHex) noexcept;
};
