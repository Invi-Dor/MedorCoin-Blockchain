#include "blockdb.h"
#include "block.h"

#include <cctype>
#include <iostream>
#include <stdexcept>

// =============================================================================
// CONSTRUCTOR / DESTRUCTOR
// Issue 1: db_ is unique_ptr — closed automatically on destruction.
// Issue 2: blockCache_ and filterPolicy_ are unique_ptr — no manual delete.
// Issue 3: stopped_ set to true before any member is destroyed —
//          prevents log calls after destruction.
// =============================================================================
BlockDB::BlockDB() = default;

BlockDB::~BlockDB() noexcept {
    // Issue 3: signal log() to stop before destroying members
    stopped_.store(true, std::memory_order_release);
    close();
    // unique_ptr members clean up automatically:
    // db_, blockCache_, filterPolicy_ all deleted here
}

// =============================================================================
// LOGGING
// Issue 3: logger copied under logMu_ then invoked outside the lock.
//          stopped_ checked first — no log after destruction.
// =============================================================================
void BlockDB::setLogger(LoggerFn fn) noexcept {
    std::lock_guard<std::mutex> lk(logMu_);
    logFn_ = std::move(fn);
}

void BlockDB::log(int level, const std::string& msg) const noexcept {
    if (stopped_.load(std::memory_order_acquire)) return;

    // Copy under lock — invoke outside lock
    LoggerFn fn;
    {
        std::lock_guard<std::mutex> lk(logMu_);
        fn = logFn_;
    }

    if (fn) {
        try { fn(level, "[BlockDB] " + msg); }
        catch (...) {}
        return;
    }
    if (level >= 2)
        std::cerr << "[BlockDB][ERROR] " << msg << "\n";
    else if (level == 1)
        std::cerr << "[BlockDB][WARN]  " << msg << "\n";
    else
        std::cout << "[BlockDB][INFO]  " << msg << "\n";
}

// =============================================================================
// INTERNAL HELPERS
// =============================================================================
BlockDB::Result BlockDB::fromStatus(const leveldb::Status& s,
                                     const std::string&     ctx) noexcept {
    if (s.ok())         return Result::success();
    if (s.IsNotFound()) return Result::failure("NOT_FOUND: " + ctx);
    return Result::failure(ctx + ": " + s.ToString());
}

// Issue 6: FORMAT-ONLY — exactly 64 lowercase hex characters.
// Does NOT check DB existence. Use hasBlock() for that.
bool BlockDB::isValidHash(const std::string& hash) noexcept {
    if (hash.size() != 64) return false;
    for (unsigned char c : hash)
        if (!std::isxdigit(c) || std::isupper(c)) return false;
    return true;
}

// =============================================================================
// OPEN
// Issue 1: db_ assigned via unique_ptr — no raw pointer ownership.
// Issue 2: blockCache_ and filterPolicy_ assigned via unique_ptr.
// Idempotent — returns success if already open.
// =============================================================================
BlockDB::Result BlockDB::open(const std::string& path) noexcept {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    if (db_)           return Result::success();
    if (path.empty())  return Result::failure("open: empty path rejected");

    try {
        // Issue 2: construct cache and filter before DB open
        blockCache_.reset(leveldb::NewLRUCache(64 * 1024 * 1024));
        filterPolicy_.reset(leveldb::NewBloomFilterPolicy(10));

        leveldb::Options opts;
        opts.create_if_missing = true;
        opts.paranoid_checks   = true;
        opts.write_buffer_size = 16 * 1024 * 1024;
        opts.max_open_files    = 1024;
        opts.block_cache       = blockCache_.get();
        opts.filter_policy     = filterPolicy_.get();

        leveldb::DB* raw = nullptr;
        leveldb::Status s = leveldb::DB::Open(opts, path, &raw);

        if (!s.ok()) {
            // Issue 2: unique_ptrs clean up cache and filter automatically
            blockCache_.reset();
            filterPolicy_.reset();
            return Result::failure(
                "open failed for '" + path + "': " + s.ToString());
        }

        // Issue 1: raw pointer immediately owned by unique_ptr
        db_.reset(raw);
        log(0, "opened at '" + path + "'");
        return Result::success();

    } catch (const std::exception& e) {
        blockCache_.reset();
        filterPolicy_.reset();
        db_.reset();
        return Result::failure(
            std::string("open: exception: ") + e.what());
    }
}

// =============================================================================
// CLOSE
// Idempotent — safe to call multiple times.
// Issue 1: db_.reset() triggers DBDeleter which calls delete on the DB.
// =============================================================================
void BlockDB::close() noexcept {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) return;
    db_.reset();
    // Issue 2: cache and filter released after DB — correct order
    blockCache_.reset();
    filterPolicy_.reset();
    log(0, "closed");
}

bool BlockDB::isOpen() const noexcept {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    return db_ != nullptr;
}

// =============================================================================
// WRITE BLOCK
// =============================================================================
BlockDB::Result BlockDB::writeBlock(const Block& block) noexcept {
    if (!isValidHash(block.hash))
        return Result::failure(
            "writeBlock: hash must be 64 lowercase hex chars, got '"
            + block.hash + "'");

    std::string value;
    try {
        value = block.serialize();
    } catch (const std::exception& e) {
        return Result::failure(
            std::string("writeBlock: serialize exception: ") + e.what());
    }
    if (value.empty())
        return Result::failure(
            "writeBlock: serialize returned empty for hash "
            + block.hash);

    leveldb::WriteOptions wo;
    wo.sync = true;

    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) return Result::failure("writeBlock: DB not open");

    try {
        leveldb::Status s = db_->Put(wo, block.hash, value);
        Result r = fromStatus(s, "writeBlock hash=" + block.hash);
        if (!r) log(2, r.error);
        return r;
    } catch (const std::exception& e) {
        return Result::failure(
            std::string("writeBlock: exception: ") + e.what());
    }
}

// =============================================================================
// READ BLOCK
// =============================================================================
BlockDB::Result BlockDB::readBlock(const std::string& hash,
                                    Block&             out) noexcept {
    if (!isValidHash(hash))
        return Result::failure(
            "readBlock: hash must be 64 lowercase hex chars");

    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) return Result::failure("readBlock: DB not open");

    std::string value;
    try {
        leveldb::Status s = db_->Get(
            leveldb::ReadOptions(), hash, &value);
        if (s.IsNotFound()) return Result::failure("NOT_FOUND");
        if (!s.ok()) return fromStatus(s, "readBlock hash=" + hash);
    } catch (const std::exception& e) {
        return Result::failure(
            std::string("readBlock: get exception: ") + e.what());
    }

    try {
        if (!out.deserialize(value))
            return Result::failure(
                "readBlock: deserialize failed for hash " + hash);
    } catch (const std::exception& e) {
        return Result::failure(
            "readBlock: deserialize exception for hash "
            + hash + ": " + e.what());
    }

    return Result::success();
}

// =============================================================================
// HAS BLOCK
// Issue 6: this is the existence check — isValidHash() is format only.
// =============================================================================
bool BlockDB::hasBlock(const std::string& hash) noexcept {
    if (!isValidHash(hash)) return false;

    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) return false;

    try {
        std::string value;
        return db_->Get(
            leveldb::ReadOptions(), hash, &value).ok();
    } catch (...) {
        return false;
    }
}

// =============================================================================
// WRITE BATCH
// Atomic write of multiple blocks in a single LevelDB WriteBatch.
// =============================================================================
BlockDB::Result BlockDB::writeBatch(
    const std::vector<Block>& blocks) noexcept
{
    if (blocks.empty()) return Result::success();

    leveldb::WriteBatch batch;

    for (size_t i = 0; i < blocks.size(); i++) {
        const Block& b = blocks[i];
        if (!isValidHash(b.hash))
            return Result::failure(
                "writeBatch: invalid hash at index "
                + std::to_string(i));

        std::string value;
        try {
            value = b.serialize();
        } catch (const std::exception& e) {
            return Result::failure(
                "writeBatch: serialize exception at index "
                + std::to_string(i) + ": " + e.what());
        }
        if (value.empty())
            return Result::failure(
                "writeBatch: empty serialization at index "
                + std::to_string(i));

        batch.Put(b.hash, value);
    }

    leveldb::WriteOptions wo;
    wo.sync = true;

    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) return Result::failure("writeBatch: DB not open");

    try {
        leveldb::Status s = db_->Write(wo, &batch);
        Result r = fromStatus(s,
            "writeBatch (" + std::to_string(blocks.size()) + " blocks)");
        if (!r) log(2, r.error);
        return r;
    } catch (const std::exception& e) {
        return Result::failure(
            std::string("writeBatch: exception: ") + e.what());
    }
}

// =============================================================================
// NEW ITERATOR
// =============================================================================
BlockDB::IteratorPtr BlockDB::newIterator() noexcept {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) return nullptr;
    try {
        return IteratorPtr(
            db_->NewIterator(leveldb::ReadOptions()));
    } catch (...) {
        return nullptr;
    }
}

// =============================================================================
// SCAN
// Issue 5: returns ScanResult with explicit count, ok flag, and error.
// =============================================================================
BlockDB::ScanResult BlockDB::scan(const ScanCallback& cb) noexcept {
    ScanResult result;

    if (!cb) {
        result.ok    = false;
        result.error = "scan: null callback rejected";
        log(2, result.error);
        return result;
    }

    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) {
        result.ok    = false;
        result.error = "scan: DB not open";
        log(2, result.error);
        return result;
    }

    try {
        IteratorPtr it(db_->NewIterator(leveldb::ReadOptions()));
        if (!it) {
            result.ok    = false;
            result.error = "scan: failed to create iterator";
            log(2, result.error);
            return result;
        }

        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            ++result.count;
            if (!cb(it->key().ToString(), it->value().ToString()))
                break;
        }

        if (!it->status().ok()) {
            result.ok    = false;
            result.error = "scan: iterator error after "
                         + std::to_string(result.count)
                         + " entries: "
                         + it->status().ToString();
            log(2, result.error);
            return result;
        }

    } catch (const std::exception& e) {
        result.ok    = false;
        result.error = std::string("scan: exception: ") + e.what();
        log(2, result.error);
        return result;
    }

    return result;
}

// =============================================================================
// ACQUIRE SNAPSHOT
// Issue 4: snapshot must not outlive this BlockDB instance.
// =============================================================================
std::unique_ptr<BlockDB::SnapshotGuard>
BlockDB::acquireSnapshot() noexcept {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) return nullptr;
    try {
        const leveldb::Snapshot* snap = db_->GetSnapshot();
        return std::make_unique<SnapshotGuard>(db_.get(), snap);
    } catch (...) {
        return nullptr;
    }
}

// =============================================================================
// READ WITH SNAPSHOT
// Issue 4: validates guard.valid() and DB open before any access.
// =============================================================================
BlockDB::Result BlockDB::readWithSnapshot(
    const std::string&   hash,
    Block&               out,
    const SnapshotGuard& guard) noexcept
{
    if (!isValidHash(hash))
        return Result::failure(
            "readWithSnapshot: hash must be 64 lowercase hex chars");

    // Issue 4: validate snapshot before use
    if (!guard.valid())
        return Result::failure(
            "readWithSnapshot: invalid or expired snapshot guard");

    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_)
        return Result::failure("readWithSnapshot: DB not open");

    leveldb::ReadOptions ro;
    ro.snapshot = guard.get();

    std::string value;
    try {
        leveldb::Status s = db_->Get(ro, hash, &value);
        if (s.IsNotFound()) return Result::failure("NOT_FOUND");
        if (!s.ok())
            return fromStatus(s,
                "readWithSnapshot hash=" + hash);
    } catch (const std::exception& e) {
        return Result::failure(
            std::string("readWithSnapshot: get exception: ")
            + e.what());
    }

    try {
        if (!out.deserialize(value))
            return Result::failure(
                "readWithSnapshot: deserialize failed for hash "
                + hash);
    } catch (const std::exception& e) {
        return Result::failure(
            "readWithSnapshot: deserialize exception: "
            + std::string(e.what()));
    }

    return Result::success();
}

// =============================================================================
// COMPACT
// =============================================================================
void BlockDB::compact() noexcept {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    if (!db_) { log(1, "compact: DB not open"); return; }
    try {
        db_->CompactRange(nullptr, nullptr);
        log(0, "compaction complete");
    } catch (const std::exception& e) {
        log(2, std::string("compact: exception: ") + e.what());
    }
}
