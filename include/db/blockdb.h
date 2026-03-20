#pragma once

#include "db/blockdb.cpp"
#include "block.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>
#include <leveldb/write_batch.h>

// =============================================================================
// BLOCKDB
//
// LevelDB-backed block storage for MedorCoin.
//
// Thread safety:
//   - All public methods are thread-safe via rwMutex (shared_mutex).
//   - Read operations use shared_lock — concurrent reads never block.
//   - Write operations use unique_lock — exclusive access for mutations.
//
// Issue 1: db_ is wrapped in a custom unique_ptr with a LevelDB deleter.
//          No raw pointer ownership — DB is closed automatically on
//          destruction even if close() is never called.
//
// Issue 2: blockCache_ and filterPolicy_ wrapped in unique_ptr with
//          custom deleters — no manual delete required anywhere.
//
// Issue 3: logger is read under shared_lock and copied before calling.
//          Logger is never called after destruction — stopped_ flag
//          checked before every log call.
//
// Issue 4: readWithSnapshot validates guard.valid() and DB open state
//          before accessing snapshot. Snapshot must not outlive BlockDB.
//
// Issue 5: scan() returns ScanResult with explicit count and status.
//          No ambiguous int64_t return value.
//
// Issue 6: isValidHash() is clearly documented as format-only check.
//          A separate hasBlock() checks existence in DB.
// =============================================================================
class BlockDB {
public:

    // =========================================================================
    // RESULT
    // =========================================================================
    struct Result {
        bool        ok    = false;
        std::string error;

        explicit operator bool() const noexcept { return ok; }

        static Result success() noexcept {
            return Result{true, {}};
        }
        static Result failure(std::string msg) noexcept {
            return Result{false, std::move(msg)};
        }
    };

    // =========================================================================
    // SCAN RESULT
    // Issue 5: explicit named result — no ambiguous int64_t return value.
    // count = number of entries visited.
    // ok    = false if iterator error occurred or DB was not open.
    // =========================================================================
    struct ScanResult {
        int64_t     count = 0;
        bool        ok    = true;
        std::string error;
    };

    // =========================================================================
    // SNAPSHOT GUARD
    // RAII — snapshot released on destruction even if caller throws.
    // Issue 4: must not outlive the BlockDB that created it.
    // =========================================================================
    class SnapshotGuard {
    public:
        SnapshotGuard(leveldb::DB*             db,
                      const leveldb::Snapshot* snap) noexcept
            : db_(db), snap_(snap) {}

        ~SnapshotGuard() noexcept {
            if (db_ && snap_) {
                db_->ReleaseSnapshot(snap_);
                snap_ = nullptr;
            }
        }

        SnapshotGuard(const SnapshotGuard&)            = delete;
        SnapshotGuard& operator=(const SnapshotGuard&) = delete;

        bool                     valid() const noexcept {
            return db_ != nullptr && snap_ != nullptr;
        }
        const leveldb::Snapshot* get() const noexcept { return snap_; }

    private:
        leveldb::DB*             db_;
        const leveldb::Snapshot* snap_;
    };

    // =========================================================================
    // CALLBACKS
    // =========================================================================
    using IteratorPtr  = std::unique_ptr<leveldb::Iterator>;
    using ScanCallback = std::function<bool(const std::string& key,
                                             const std::string& value)>;
    using LoggerFn     = std::function<void(int                level,
                                             const std::string& msg)>;

    // =========================================================================
    // LIFECYCLE
    // =========================================================================
    BlockDB();
    ~BlockDB();

    BlockDB(const BlockDB&)            = delete;
    BlockDB& operator=(const BlockDB&) = delete;

    Result open (const std::string& path) noexcept;
    void   close()                        noexcept;
    bool   isOpen()                 const noexcept;

    // =========================================================================
    // LOGGER
    // Issue 3: logger stored under write lock.
    //          All log calls copy the function under shared_lock then
    //          invoke it outside the lock — safe against concurrent
    //          destruction and logger replacement.
    // =========================================================================
    void setLogger(LoggerFn fn) noexcept;

    // =========================================================================
    // BLOCK OPERATIONS
    // All hash parameters must be exactly 64 lowercase hex characters.
    // Issue 6: isValidHash() checks FORMAT only — not DB existence.
    //          Use hasBlock() to check existence in DB.
    // =========================================================================
    Result writeBlock (const Block&              block)  noexcept;
    Result readBlock  (const std::string&        hash,
                        Block&                    out)    noexcept;
    bool   hasBlock   (const std::string&        hash)   noexcept;
    Result writeBatch (const std::vector<Block>& blocks) noexcept;

    // =========================================================================
    // ITERATION
    // Issue 5: scan() returns ScanResult — count + ok + error string.
    // =========================================================================
    IteratorPtr newIterator()                    noexcept;
    ScanResult  scan(const ScanCallback& cb)     noexcept;

    // =========================================================================
    // SNAPSHOT
    // Issue 4: snapshot must not outlive this BlockDB instance.
    //          readWithSnapshot validates guard.valid() before use.
    // =========================================================================
    std::unique_ptr<SnapshotGuard> acquireSnapshot()                  noexcept;
    Result                         readWithSnapshot(
                                       const std::string&   hash,
                                       Block&               out,
                                       const SnapshotGuard& guard)    noexcept;

    // =========================================================================
    // MAINTENANCE
    // =========================================================================
    void compact() noexcept;

private:

    // Issue 1: RAII DB ownership via unique_ptr with custom deleter
    struct DBDeleter {
        void operator()(leveldb::DB* p) const noexcept {
            delete p;
        }
    };

    // Issue 2: RAII cache and filter ownership
    struct CacheDeleter {
        void operator()(leveldb::Cache* p) const noexcept {
            delete p;
        }
    };
    struct FilterDeleter {
        void operator()(const leveldb::FilterPolicy* p) const noexcept {
            delete p;
        }
    };

    std::unique_ptr<leveldb::DB,
        DBDeleter>                         db_;
    std::unique_ptr<leveldb::Cache,
        CacheDeleter>                      blockCache_;
    std::unique_ptr<const leveldb::FilterPolicy,
        FilterDeleter>                     filterPolicy_;

    mutable std::shared_mutex              rwMutex_;

    // Issue 3: logger protected by logMu_ — separate from rwMutex_
    // so logger replacement never blocks DB reads/writes.
    mutable std::mutex                     logMu_;
    LoggerFn                               logFn_;

    // Issue 3: stopped_ prevents log calls after destruction
    std::atomic<bool>                      stopped_{false};

    // Internal helpers
    void   log(int level, const std::string& msg) const noexcept;
    Result fromStatus(const leveldb::Status& s,
                       const std::string&     ctx)        noexcept;

    // Issue 6: FORMAT-ONLY check — 64 lowercase hex characters.
    // Does NOT check DB existence. Use hasBlock() for that.
    static bool isValidHash(const std::string& hash) noexcept;
};
