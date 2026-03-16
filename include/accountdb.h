#pragma once

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/status.h>

#include <functional>
#include <shared_mutex>
#include <string>
#include <vector>

/**
 * AccountDB
 *
 * Production-grade RocksDB wrapper for persistent account state storage.
 *
 * Design guarantees:
 *  - Result type is structurally identical to BlockDB::Result and
 *    RocksDBWrapper::Result so all storage layers report errors uniformly.
 *  - A single database-wide shared_mutex governs all operations.
 *    Readers hold shared locks and never block each other. Writers hold
 *    exclusive locks. RocksDB's own internal WriteBatch atomicity guarantees
 *    correctness for batch operations across all keys without any risk of
 *    partial cross-stripe writes that stripe-based locking would introduce.
 *  - sync defaults to true on all writes so durability is the safe default
 *    and unsafe usage is visible at every call site.
 *  - Keys are validated for emptiness, maximum length, and the absence of
 *    null bytes and ASCII control characters before any DB operation.
 *  - iteratePrefix acquires a shared read lock over the entire DB for the
 *    duration of the scan, preventing any writer from modifying keys that
 *    the iterator has not yet reached.
 *  - WAL is explicitly flushed in the destructor before the handle is
 *    released; if the flush fails it is retried once before logging a warning.
 *  - All public methods are noexcept. No method throws.
 *  - The logger is replaceable at runtime so std::cerr is never hardwired
 *    into production paths.
 */
class AccountDB {
public:

    // ── Result — identical shape to BlockDB::Result / RocksDBWrapper::Result ──
    struct Result {
        bool        ok = false;
        std::string error;

        explicit operator bool() const noexcept { return ok; }

        static Result success()
            { return {true,  {}}; }
        static Result failure(const std::string &msg)
            { return {false, msg}; }
    };

    // ── Replaceable logger — level: 0 = info, 1 = warn, 2 = error ────────────
    using LogFn = std::function<void(int level, const std::string &msg)>;
    void setLogger(LogFn fn) noexcept { logger = std::move(fn); }

    // ── Streaming scan callback — return true to continue, false to stop ──────
    using ScanCallback = std::function<bool(const std::string &key,
                                            const std::string &value)>;

    explicit AccountDB(const std::string &path);
    ~AccountDB();

    AccountDB(const AccountDB &)            = delete;
    AccountDB &operator=(const AccountDB &) = delete;

    // ── Liveness ──────────────────────────────────────────────────────────────
    bool isOpen()    const noexcept;
    bool isHealthy() const noexcept;

    // ── Core operations ───────────────────────────────────────────────────────
    Result put(const std::string &key,
               const std::string &value,
               bool sync = true)  noexcept;

    Result get(const std::string &key,
               std::string       &valueOut) noexcept;

    Result del(const std::string &key,
               bool sync = true)  noexcept;

    // ── Atomic batch operations ───────────────────────────────────────────────
    // All items are validated before the batch is submitted. If any item is
    // invalid the entire batch is rejected and nothing is written.
    Result writeBatch(
        const std::vector<std::pair<std::string, std::string>> &items,
        bool sync = true) noexcept;

    Result deleteBatch(
        const std::vector<std::string> &keys,
        bool sync = true) noexcept;

    // ── Streaming prefix scan ─────────────────────────────────────────────────
    // Acquires a shared lock for the full duration of the scan so no writer
    // can modify keys the iterator has not yet visited. Returns the number of
    // entries visited, or -1 on error.
    int64_t iteratePrefix(const std::string  &prefix,
                          const ScanCallback &callback,
                          size_t              maxResults = 0) noexcept;

private:
    rocksdb::DB      *db = nullptr;
    rocksdb::Options  options;
    LogFn             logger;

    // Single database-wide shared_mutex — eliminates cross-stripe atomicity
    // ambiguity while still allowing full read concurrency.
    mutable std::shared_mutex dbMutex;

    static constexpr const char *HEALTH_KEY = "__accountdb_health_probe__";
    static constexpr size_t      MAX_KEY_LEN = 512;

    void   log(int level, const std::string &msg) const noexcept;

    static Result fromStatus(const rocksdb::Status &s,
                              const std::string     &ctx) noexcept;
    static bool   isValidKey (const std::string     &key) noexcept;
};
