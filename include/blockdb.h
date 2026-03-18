#pragma once

#include "block.h"
#include <leveldb/db.h>
#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>
#include <leveldb/write_batch.h>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

class BlockDB {
public:
    struct Result {
        bool        ok    = true;
        std::string error;
        static Result success()                  { return {true,  ""}; }
        static Result failure(const std::string& e) { return {false, e}; }
        explicit operator bool() const { return ok; }
    };

    using IteratorPtr   = std::unique_ptr<leveldb::Iterator>;
    using ScanCallback  = std::function<bool(const std::string&,
                                              const std::string&)>;
    using LoggerFn      = std::function<void(int, const std::string&)>;

    struct SnapshotGuard {
        SnapshotGuard(leveldb::DB* db, const leveldb::Snapshot* snap)
            : db_(db), snap_(snap) {}
        ~SnapshotGuard() { if (db_ && snap_) db_->ReleaseSnapshot(snap_); }
        bool valid()                     const { return snap_ != nullptr; }
        const leveldb::Snapshot* get()   const { return snap_; }
        SnapshotGuard(const SnapshotGuard&)            = delete;
        SnapshotGuard& operator=(const SnapshotGuard&) = delete;
    private:
        leveldb::DB*              db_;
        const leveldb::Snapshot*  snap_;
    };

    BlockDB();
    ~BlockDB();
    BlockDB(const BlockDB&)            = delete;
    BlockDB& operator=(const BlockDB&) = delete;

    Result open(const std::string& path) noexcept;
    void   close()                       noexcept;
    bool   isOpen()                const noexcept;

    Result      writeBlock(const Block& block)                          noexcept;
    Result      readBlock(const std::string& hash, Block& blockOut)     noexcept;
    bool        hasBlock(const std::string& hash)                       noexcept;
    Result      writeBatch(const std::vector<Block>& blocks)            noexcept;
    IteratorPtr newIterator()                                            noexcept;
    int64_t     scan(const ScanCallback& callback)                      noexcept;

    std::unique_ptr<SnapshotGuard> acquireSnapshot()                    noexcept;
    Result readWithSnapshot(const std::string& hash,
                            Block& blockOut,
                            const SnapshotGuard& guard)                 noexcept;
    void compact()                                                       noexcept;

    void setLogger(LoggerFn fn) { logger = std::move(fn); }

private:
    leveldb::DB*                  db           = nullptr;
    leveldb::Cache*               blockCache   = nullptr;
    const leveldb::FilterPolicy*  filterPolicy = nullptr;
    mutable std::shared_mutex     rwMutex;
    LoggerFn                      logger;

    void   log(int level, const std::string& msg)              const noexcept;
    Result fromStatus(const leveldb::Status& s,
                      const std::string& ctx)                        noexcept;
    static bool isValidHash(const std::string& hash)                 noexcept;
};
