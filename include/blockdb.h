#pragma once

#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/write_batch.h>
#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>
#include <leveldb/snapshot.h>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

struct Block;

/**
 * BlockDB
 *
 * Production-grade LevelDB wrapper for persistent block storage.
 *
 * All hash inputs are validated as exactly 64 lowercase hex characters
 * (SHA-256 format). Snapshots are managed via RAII SnapshotGuard to
 * eliminate any possibility of snapshot leaks. All LevelDB calls are
 * wrapped in try/catch. Logging is routed through a replaceable logger
 * so std::cerr is never hardwired into production paths.
 */
class BlockDB {
public:

    // ── Result ────────────────────────────────────────────────────────────────
    struct Result {
        bool        ok = false;
        std::string error;
        explicit operator bool() const noexcept { return ok; }
        static Result success()                          { return {true,  {}}; }
        static Result failure(const std::string &msg)   { return {false, msg}; }
    };

    // ── Logging ───────────────────────────────────────────────────────────────
    // Replace this with spdlog, glog, or any framework at startup.
    // Signature: void(level, message)  where level: 0=info 1=warn 2=error
    using LogFn = std::function<void(int level, const std::string &msg)>;
    void setLogger(LogFn fn) noexcept { logger = std::move(fn); }

    // ── Iterator ──────────────────────────────────────────────────────────────
    using IteratorPtr  = std::unique_ptr<leveldb::Iterator>;
    using ScanCallback = std::function<bool(const std::string &key,
                                            const std::string &value)>;

    // ── RAII snapshot guard — snapshot is always released on destruction ──────
    class SnapshotGuard {
    public:
        SnapshotGuard(leveldb::DB *db, const leveldb::Snapshot *snap)
            : db_(db), snap_(snap) {}
        ~SnapshotGuard() { if (db_ && snap_) db_->ReleaseSnapshot(snap_); }

        SnapshotGuard(const SnapshotGuard &)            = delete;
        SnapshotGuard &operator=(const SnapshotGuard &) = delete;

        const leveldb::Snapshot *get() const noexcept { return snap_; }
        bool valid()                    const noexcept { return snap_ != nullptr; }

    private:
        leveldb::DB             *db_;
        const leveldb::Snapshot *snap_;
    };

    BlockDB();
    ~BlockDB();

    BlockDB(const BlockDB &)            = delete;
    BlockDB &operator=(const BlockDB &) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    Result open (const std::string &path) noexcept;
    void   close()                        noexcept;
    bool   isOpen()                 const noexcept;

    // ── Single block ──────────────────────────────────────────────────────────
    Result writeBlock(const Block &block)                        noexcept;
    Result readBlock (const std::string &hash, Block &blockOut) noexcept;
    bool   hasBlock  (const std::string &hash)                  noexcept;

    // ── Atomic batch ──────────────────────────────────────────────────────────
    Result writeBatch(const std::vector<Block> &blocks)         noexcept;

    // ── Iteration ─────────────────────────────────────────────────────────────
    IteratorPtr newIterator()                                    noexcept;
    int64_t     scan(const ScanCallback &callback)              noexcept;

    // ── Snapshot ──────────────────────────────────────────────────────────────
    // Returns a RAII guard. The snapshot is released automatically when
    // the guard goes out of scope — callers cannot forget to release it.
    std::unique_ptr<SnapshotGuard> acquireSnapshot()            noexcept;

    Result readWithSnapshot(const std::string  &hash,
                             Block              &blockOut,
                             const SnapshotGuard &guard)        noexcept;

    // ── Maintenance ───────────────────────────────────────────────────────────
    void compact() noexcept;

private:
    leveldb::DB                 *db           = nullptr;
    leveldb::Cache              *blockCache   = nullptr;
    const leveldb::FilterPolicy *filterPolicy = nullptr;

    mutable std::shared_mutex rwMutex;
    LogFn                     logger;

    void log(int level, const std::string &msg) const noexcept;

    static Result fromStatus(const leveldb::Status &s,
                              const std::string     &ctx) noexcept;

    // Enforces exactly 64 lowercase hex characters — SHA-256 format
    static bool isValidHash(const std::string &hash) noexcept;
};
