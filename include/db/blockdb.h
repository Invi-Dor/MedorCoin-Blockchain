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

class BlockDB {
public:

    struct Result {
        bool        ok    = false;
        std::string error;
        explicit operator bool() const noexcept { return ok; }
        static Result success() noexcept { return Result{true, {}}; }
        static Result failure(std::string msg) noexcept {
            return Result{false, std::move(msg)};
        }
    };

    struct ScanResult {
        int64_t     count = 0;
        bool        ok    = true;
        std::string error;
    };

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

    using IteratorPtr  = std::unique_ptr<leveldb::Iterator>;
    using ScanCallback = std::function<bool(const std::string& key,
                                             const std::string& value)>;
    using LoggerFn     = std::function<void(int                level,
                                             const std::string& msg)>;

    BlockDB();
    ~BlockDB();

    BlockDB(const BlockDB&)            = delete;
    BlockDB& operator=(const BlockDB&) = delete;

    Result open (const std::string& path) noexcept;
    void   close()                        noexcept;
    bool   isOpen()                 const noexcept;

    void setLogger(LoggerFn fn) noexcept;

    Result      writeBlock (const Block&              block)  noexcept;
    Result      readBlock  (const std::string&        hash,
                             Block&                    out)    noexcept;
    bool        hasBlock   (const std::string&        hash)   noexcept;
    Result      writeBatch (const std::vector<Block>& blocks) noexcept;

    IteratorPtr newIterator()                noexcept;
    ScanResult  scan(const ScanCallback& cb) noexcept;

    std::unique_ptr<SnapshotGuard> acquireSnapshot() noexcept;
    Result readWithSnapshot(const std::string&   hash,
                             Block&               out,
                             const SnapshotGuard& guard) noexcept;

    void compact() noexcept;

private:

    struct DBDeleter {
        void operator()(leveldb::DB* db) const noexcept {
            delete db;
        }
    };

    struct CacheDeleter {
        void operator()(leveldb::Cache* c) const noexcept {
            delete c;
        }
    };

    struct FilterDeleter {
        void operator()(const leveldb::FilterPolicy* f) const noexcept {
            delete f;
        }
    };

    std::unique_ptr<leveldb::DB,
                    DBDeleter>                        db_;
    std::unique_ptr<leveldb::Cache,
                    CacheDeleter>                     blockCache_;
    std::unique_ptr<const leveldb::FilterPolicy,
                    FilterDeleter>                    filterPolicy_;

    mutable std::shared_mutex                         rwMutex_;
    std::mutex                                        logMu_;
    LoggerFn                                          logFn_;
    std::atomic<bool>                                 stopped_;

    void log(int level, const std::string& msg) noexcept;

    static bool        isValidHash(const std::string& hash) noexcept;
    static std::string blockKey   (const std::string& hash) noexcept;
};
