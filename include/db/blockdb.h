#pragma once

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
//   All public methods are thread-safe via rwMutex_ (shared_mutex).
//   Reads use shared_lock -- concurrent reads never block each other.
//   Writes use unique_lock -- exclusive access during mutations.
//   Logger is copied under logMu_ before calling -- safe against
//   concurrent destruction and logger replacement.
//   stopped_ prevents log calls after destruction.
//
// Production / deployment:
//   64MB LRU block cache and Bloom filter configured at open time.
//   32MB write buffer for sustained write throughput.
//   Up to 500 open files for large chain depths.
//   writeBatch() for atomic multi-block commits -- no partial writes.
//   acquireSnapshot() for consistent point-in-time reads under load.
//   compact() to reclaim space and improve read performance on mainnet.
//   All methods return Result -- never throw into caller code.
//
// Issue 1: db_ wrapped in unique_ptr with custom deleter -- DB closed
//   automatically on destruction even if close() is never called.
// Issue 2: blockCache_ and filterPolicy_ wrapped in unique_ptr with
//   custom deleters -- no manual delete required anywhere.
// Issue 3: stopped_ explicitly initialized to false in constructor.
//   log() null-checks logFn_ before calling -- safe when no logger set.
//   stopped_ checked before every log call -- never called after destruction.
// Issue 4: readWithSnapshot validates guard.valid() before any DB access.
//   Caller must not pass a guard that outlives its BlockDB instance.
// Issue 5: scan() returns ScanResult with explicit count and status.
// Issue 6: isValidHash() is format-only -- 64 lowercase hex characters.
//   Use hasBlock() to check DB existence.
// =============================================================================

class BlockDB {
public:

    // =========================================================================
    // RESULT
    // Returned by all write and open operations.
    // operator bool() allows: if (!result) { ... }
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
    // Returned by scan() -- explicit count and status, no ambiguous return.
    // =========================================================================
    struct ScanResult {
        int64_t     count = 0;
        bool        ok    = true;
        std::string error;
    };

    // =========================================================================
    // SNAPSHOT GUARD
    // RAII wrapper around leveldb::Snapshot.
    // Snapshot released automatically on destruction.
    // Must not outlive the BlockDB instance that created it.
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
        const leveldb::Snapshot* get()   const noexcept { return snap_; }

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
    // stopped_ explicitly initialized to false in constructor.
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
    // Logger stored and invoked outside rwMutex_ so heavy log callbacks
    // never block DB reads or writes.
    // logFn_ default-initialized to empty -- log() handles null safely.
    // =========================================================================
    void setLogger(LoggerFn fn) noexcept;

    // =========================================================================
    // BLOCK OPERATIONS
    // writeBlock  -- single block atomic write
    // readBlock   -- deserialize block by hash
    // hasBlock    -- existence check without deserialization
    // writeBatch  -- atomic multi-block write, no partial commits
    // =========================================================================
    Result writeBlock (const Block&              block)  noexcept;
    Result readBlock  (const std::string&        hash,
                        Block&                    out)    noexcept;
    bool   hasBlock   (const std::string&        hash)   noexcept;
    Result writeBatch (const std::vector<Block>& blocks) noexcept;

    // =========================================================================
    // ITERATION
    // newIterator -- raw iterator for full chain traversal
    // scan        -- callback-based traversal with early exit support
    // =========================================================================
    IteratorPtr newIterator()                noexcept;
    ScanResult  scan(const ScanCallback& cb) noexcept;

    // =========================================================================
    // SNAPSHOT
    // acquireSnapshot  -- point-in-time consistent read handle
    // readWithSnapshot -- read block under an acquired snapshot
    //   guard must remain valid for the lifetime of the call.
    //   guard must not outlive the BlockDB instance that created it.
    // =========================================================================
    std::unique_ptr<SnapshotGuard> acquireSnapshot()                       noexcept;
    Result                         readWithSnapshot(const std::string&   hash,
                                                     Block&               out,
                                                     const SnapshotGuard& guard) noexcept;

    // =========================================================================
    // MAINTENANCE
    // compact() triggers LevelDB range compaction to reclaim disk space
    // and improve read latency on mainnet after heavy write periods.
    // =========================================================================
    void compact() noexcept;

private:

    // RAII deleters -- no manual delete anywhere in the codebase
    struct DB​​​​​​​​​​​​​​​​
