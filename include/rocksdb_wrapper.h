#pragma once

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/status.h>
#include <rocksdb/write_batch.h>

#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

/**
 * RocksDBWrapper
 *
 * Production-grade wrapper around RocksDB providing:
 *
 *  - Full thread-safety via a 128-stripe shared_mutex array so concurrent
 *    readers on different keys never block each other and writers only
 *    contend within their stripe.
 *  - Explicit error reporting via a Result type that carries the full
 *    RocksDB Status string on failure, so callers are never left with a
 *    silent bool=false and no context.
 *  - Sync writes default to TRUE so callers must explicitly opt out of
 *    durability rather than accidentally opting in.
 *  - A streaming iterator interface for prefix scans so large result sets
 *    are never forced into a single heap allocation.
 *  - A liveness check beyond db != nullptr: a background health probe
 *    writes and reads a sentinel key to confirm the DB is fully writable.
 *  - All public methods are noexcept; RocksDB exceptions are caught
 *    internally and converted to error Results.
 */
class RocksDBWrapper {
public:

    // Carries success/failure and, on failure, the full RocksDB Status message.
    struct Result {
        bool        ok      = false;
        std::string error;          // empty when ok == true

        explicit operator bool() const noexcept { return ok; }

        static Result success()
        {
            Result r; r.ok = true; return r;
        }
        static Result failure(const std::string &msg)
        {
            Result r; r.ok = false; r.error = msg; return r;
        }
    };

    // Callback signature used by the streaming prefix iterator.
    // Return true to continue iteration, false to stop early.
    using IteratorCallback =
        std::function<bool(const std::string &key,
                           const std::string &value)>;

    explicit RocksDBWrapper(const std::string &path);
    ~RocksDBWrapper();

    RocksDBWrapper(const RocksDBWrapper &)            = delete;
    RocksDBWrapper &operator=(const RocksDBWrapper &) = delete;

    // ── Liveness ──────────────────────────────────────────────────────────────

    // Returns true only if the DB handle is open AND a write+read probe
    // on a reserved sentinel key succeeds. Use this for health checks.
    bool isOpen()    const noexcept;
    bool isHealthy() const noexcept;

    // ── Core operations ───────────────────────────────────────────────────────

    // sync defaults to true — callers must explicitly pass false to disable
    // durability, making unsafe usage visible at the call site.
    Result put(const std::string &key,
               const std::string &value,
               bool sync = true)  noexcept;

    Result get(const std::string &key,
               std::string &valueOut) noexcept;

    Result del(const std::string &key,
               bool sync = true)  noexcept;

    // ── Streaming prefix scan ─────────────────────────────────────────────────

    // Invokes callback(key, value) for every key that begins with prefix.
    // Iteration stops when the prefix no longer matches, maxResults is
    // reached (0 = unlimited), or the callback returns false.
    // Returns the number of entries visited, or -1 on iterator error.
    int64_t iteratePrefix(const std::string     &prefix,
                          const IteratorCallback &callback,
                          size_t                  maxResults = 0) noexcept;

    // ── Atomic batch operations ───────────────────────────────────────────────

    Result batchPut   (const std::vector<std::pair<std::string,
                                                    std::string>> &items,
                       bool sync = true)  noexcept;

    Result batchDelete(const std::vector<std::string> &keys,
                       bool sync = true)  noexcept;

private:
    rocksdb::DB      *db = nullptr;
    rocksdb::Options  options;

    // 128-stripe shared_mutex — readers on different stripes never contend;
    // writers only block within their own stripe.
    static constexpr size_t STRIPE_COUNT = 128;
    mutable std::shared_mutex stripes[STRIPE_COUNT];

    size_t stripeFor(const std::string &key) const noexcept;

    // Sentinel key used by isHealthy()
    static constexpr const char *HEALTH_KEY = "__rocksdb_health_probe__";
};
