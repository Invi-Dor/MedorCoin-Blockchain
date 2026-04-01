#include "db/blockdb.h"

#include <iostream>
#include <sstream>
#include <iomanip>

BlockDB::BlockDB()
    : stopped_(false)
{
}

BlockDB::~BlockDB()
{
    stopped_.store(true);
    close();
}

void BlockDB::setLogger(LoggerFn fn) noexcept
{
    std::lock_guard<std::mutex> lk(logMu_);
    logFn_ = std::move(fn);
}

void BlockDB::log(int level, const std::string& msg) const noexcept
{
    if (stopped_.load()) return;
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
    // fn is null -- fall back to stderr for level >= 1 only
    if (level >= 1)
        std::cerr << "[BlockDB] " << msg << "\n";
}

BlockDB::Result BlockDB::open(const std::string& path) noexcept
{
    std::unique_lock<std::shared_mutex> lk(rwMutex_);
    if (db_)
        return Result::failure("already open");
    try {
        blockCache_.reset(leveldb::NewLRUCache(64 * 1024 * 1024));
        filterPolicy_.reset(leveldb::NewBloomFilterPolicy(10));
        leveldb::Options opts;
        opts.create_if_missing = true;
        opts.block_cache       = blockCache_.get();
        opts.filter_policy     = filterPolicy_.get();
        opts.write_buffer_size = 32 * 1024 * 1024;
        opts.max_open_files    = 500;
        leveldb::DB* raw = nullptr;
        leveldb::Status s = leveldb::DB::Open(opts, path, &raw);
        if (!s.ok())
            return Result::failure("leveldb open failed: " + s.ToString());
        db_.reset(raw);
        log(0, "opened at " + path);
        return Result::success();
    } catch (const std::exception& e) {
        return Result::failure(std::string("exception during open: ") + e.what());
    } catch (...) {
        return Result::failure("unknown exception during open");
    }
}

void BlockDB::close() noexcept
{
    std::unique_lock<std::shared_mutex> lk(rwMutex_);
    db_.reset();
    blockCache_.reset();
    filterPolicy_.reset();
    log(0, "closed");
}

bool BlockDB::isOpen() const noexcept
{
    std::shared_lock<std::shared_mutex> lk(rwMutex_);
    return db_ != nullptr;
}

BlockDB::Result BlockDB::fromStatus(const leveldb::Status& s,
                                     const std::string&     ctx) noexcept
{
    if (s.ok()) return Result::success();
    return Result::failure(ctx + ": " + s.ToString());
}

bool BlockDB::isValidHash(const std::string& hash) noexcept
{
    if (hash.size() != 64) return false;
    for (char c : hash) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

BlockDB::Result BlockDB::writeBlock(const Block& block) noexcept
{
    if (!isValidHash(block.hash))
        return Result::failure("writeBlock: invalid hash format");
    std::unique_lock<std::shared_mutex> lk(rwMutex_);
    if (!db_) return Result::failure("writeBlock: DB not open");
    try {
        std::string serialized = block.serialize();
        leveldb::Status s = db_->Put(leveldb::WriteOptions(),
                                      block.hash, serialized);
        if (!s.ok()) {
            log(1, "writeBlock failed for " + block.hash + ": " + s.ToString());
            return fromStatus(s, "writeBlock");
        }
        log(0, "wrote block " + block.hash);
        return Result::success();
    } catch (const std::exception& e) {
        return Result::failure(std::string("writeBlock exception: ") + e.what());
    } catch (...) {
        return Result::failure("writeBlock: unknown exception");
    }
}

BlockDB::Result BlockDB::readBlock(const std::string& hash, Block& out) noexcept
{
    if (!isValidHash(hash))
        return Result::failure("readBlock: invalid hash format");
    std::shared_lock<std::shared_mutex> lk(rwMutex_);
    if (!db_) return Result::failure("readBlock: DB not open");
    try {
        std::string raw;
        leveldb::Status s = db_->Get(leveldb::ReadOptions(), hash, &raw);
        if (!s.ok()) return fromStatus(s, "readBlock");
        if (!out.deserialize(raw))
            return Result::failure("readBlock: deserialize failed for " + hash);
        return Result::success();
    } catch (const std::exception& e) {
        return Result::failure(std::string("readBlock exception: ") + e.what());
    } catch (...) {
        return Result::failure("readBlock: unknown exception");
    }
}

bool BlockDB::hasBlock(const std::string& hash) noexcept
{
    if (!isValidHash(hash)) return false;
    std::shared_lock<std::shared_mutex> lk(rwMutex_);
    if (!db_) return false;
    std::string val;
    leveldb::Status s = db_->Get(leveldb::ReadOptions(), hash, &val);
    return s.ok();
}

BlockDB::Result BlockDB::writeBatch(const std::vector<Block>& blocks) noexcept
{
    std::unique_lock<std::shared_mutex> lk(rwMutex_);
    if (!db_) return Result::failure("writeBatch: DB not open");
    try {
        leveldb::WriteBatch batch;
        for (const auto& block : blocks) {
            if (!isValidHash(block.hash))
                return Result::failure("writeBatch: invalid hash " + block.hash);
            batch.Put(block.hash, block.serialize());
        }
        leveldb::Status s = db_->Write(leveldb::WriteOptions(), &batch);
        if (!s.ok()) {
            log(1, "writeBatch failed: " + s.ToString());
            return fromStatus(s, "writeBatch");
        }
        log(0, "writeBatch wrote " + std::to_string(blocks.size()) + " blocks");
        return Result::success();
    } catch (const std::exception& e) {
        return Result::failure(std::string("writeBatch exception: ") + e.what());
    } catch (...) {
        return Result::failure("writeBatch: unknown exception");
    }
}

BlockDB::IteratorPtr BlockDB::newIterator() noexcept
{
    std::shared_lock<std::shared_mutex> lk(rwMutex_);
    if (!db_) return nullptr;
    return IteratorPtr(db_->NewIterator(leveldb::ReadOptions()));
}

BlockDB::ScanResult BlockDB::scan(const ScanCallback& cb) noexcept
{
    ScanResult result;
    std::shared_lock<std::shared_mutex> lk(rwMutex_);
    if (!db_) {
        result.ok    = false;
        result.error = "scan: DB not open";
        return result;
    }
    try {
        IteratorPtr it(db_->NewIterator(leveldb::ReadOptions()));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            bool cont = cb(it->key().ToString(), it->value().ToString());
            ++result.count;
            if (!cont) break;
        }
        if (!it->status().ok()) {
            result.ok    = false;
            result.error = "scan: iterator error: " + it->status().ToString();
        }
    } catch (const std::exception& e) {
        result.ok    = false;
        result.error = std::string("scan exception: ") + e.what();
    } catch (...) {
        result.ok    = false;
        result.error = "scan: unknown exception";
    }
    return result;
}

std::unique_ptr<BlockDB::SnapshotGuard> BlockDB::acquireSnapshot() noexcept
{
    std::shared_lock<std::shared_mutex> lk(rwMutex_);
    if (!db_) return nullptr;
    const leveldb::Snapshot* snap = db_->GetSnapshot();
    return std::make_unique<SnapshotGuard>(db_.get(), snap);
}

BlockDB::Result BlockDB::readWithSnapshot(const std::string&   hash,
                                           Block&               out,
                                           const SnapshotGuard& guard) noexcept
{
    // Fix 3: explicit lifetime and validity check before any DB access.
    // Caller must not pass a guard that outlives its BlockDB instance.
    if (!guard.valid())
        return Result::failure("readWithSnapshot: guard is invalid or snapshot"
                               " has already been released -- check lifetime");
    if (!isValidHash(hash))
        return Result::failure("readWithSnapshot: invalid hash format");
    std::shared_lock<std::shared_mutex> lk(rwMutex_);
    if (!db_) return Result::failure("readWithSnapshot: DB not open");
    try {
        leveldb::ReadOptions opts;
        opts.snapshot = guard.get();
        std::string raw;
        leveldb::Status s = db_->Get(opts, hash, &raw);
        if (!s.ok()) return fromStatus(s, "readWithSnapshot");
        if (!out.deserialize(raw))
            return Result::failure("readWithSnapshot: deserialize failed for "
                                   + hash);
        return Result::success();
    } catch (const std::exception& e) {
        return Result::failure(std::string("readWithSnapshot exception: ")
                               + e.what());
    } catch (...) {
        return Result::failure("readWithSnapshot: unknown exception");
    }
}

void BlockDB::compact() noexcept
{
    std::shared_lock<std::shared_mutex> lk(rwMutex_);
    if (!db_) return;
    db_->CompactRange(nullptr, nullptr);
    log(0, "compaction triggered");
}
