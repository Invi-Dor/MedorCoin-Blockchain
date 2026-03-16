#include "db/blockdb.h"
#include "block.h"

#include <cctype>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// Logging
// ─────────────────────────────────────────────────────────────────────────────

void BlockDB::log(int level, const std::string &msg) const noexcept
{
    if (logger) {
        try { logger(level, msg); } catch (...) {}
        return;
    }
    // Default fallback — replace logger via setLogger() in production
    if (level >= 2)
        std::cerr << "[BlockDB][ERROR] " << msg << "\n";
    else if (level == 1)
        std::cerr << "[BlockDB][WARN]  " << msg << "\n";
    else
        std::cout << "[BlockDB][INFO]  " << msg << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

BlockDB::Result BlockDB::fromStatus(const leveldb::Status &s,
                                     const std::string     &ctx) noexcept
{
    if (s.ok())         return Result::success();
    if (s.IsNotFound()) return Result::failure("NOT_FOUND: " + ctx);
    return Result::failure(ctx + ": " + s.ToString());
}

bool BlockDB::isValidHash(const std::string &hash) noexcept
{
    // Enforces exactly 64 lowercase hex characters (SHA-256)
    if (hash.size() != 64) return false;
    for (unsigned char c : hash)
        if (!std::isxdigit(c) || std::isupper(c)) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

BlockDB::BlockDB() = default;

BlockDB::~BlockDB()
{
    close();
    // LevelDB does not take ownership of cache or filter policy.
    // We only delete them if they were successfully allocated; nullptr
    // delete is safe in C++ but the explicit check makes intent clear.
    if (blockCache)   { delete blockCache;   blockCache   = nullptr; }
    if (filterPolicy) { delete filterPolicy; filterPolicy = nullptr; }
}

// ─────────────────────────────────────────────────────────────────────────────
// Open
// ─────────────────────────────────────────────────────────────────────────────

BlockDB::Result BlockDB::open(const std::string &path) noexcept
{
    std::unique_lock<std::shared_mutex> lock(rwMutex);

    if (db)         return Result::success();  // idempotent
    if (path.empty()) return Result::failure("open: empty path rejected");

    try {
        blockCache   = leveldb::NewLRUCache(64 * 1024 * 1024);
        filterPolicy = leveldb::NewBloomFilterPolicy(10);

        leveldb::Options opts;
        opts.create_if_missing  = true;
        opts.paranoid_checks    = true;
        opts.write_buffer_size  = 16 * 1024 * 1024;
        opts.max_open_files     = 1024;
        opts.block_cache        = blockCache;
        opts.filter_policy      = filterPolicy;

        leveldb::DB    *tempDb = nullptr;
        leveldb::Status s      = leveldb::DB::Open(opts, path, &tempDb);

        if (!s.ok()) {
            delete blockCache;   blockCache   = nullptr;
            delete filterPolicy; filterPolicy = nullptr;
            return Result::failure("open failed for '" + path
                                   + "': " + s.ToString());
        }

        db = tempDb;
        log(0, "Opened at '" + path + "'");
        return Result::success();

    } catch (const std::exception &e) {
        if (blockCache)   { delete blockCache;   blockCache   = nullptr; }
        if (filterPolicy) { delete filterPolicy; filterPolicy = nullptr; }
        return Result::failure(std::string("open: exception: ") + e.what());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Close
// ─────────────────────────────────────────────────────────────────────────────

void BlockDB::close() noexcept
{
    std::unique_lock<std::shared_mutex> lock(rwMutex);
    if (db) {
        delete db;
        db = nullptr;
        log(0, "Closed");
    }
}

bool BlockDB::isOpen() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(rwMutex);
    return db != nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Write single block
// ─────────────────────────────────────────────────────────────────────────────

BlockDB::Result BlockDB::writeBlock(const Block &block) noexcept
{
    if (!isValidHash(block.hash))
        return Result::failure(
            "writeBlock: hash must be exactly 64 lowercase hex chars, got '"
            + block.hash + "'");

    std::string value;
    try {
        value = block.serialize();
    } catch (const std::exception &e) {
        return Result::failure(
            std::string("writeBlock: serialize exception: ") + e.what());
    }

    if (value.empty())
        return Result::failure(
            "writeBlock: serialize returned empty string for hash "
            + block.hash);

    leveldb::WriteOptions wo;
    wo.sync = true;

    std::unique_lock<std::shared_mutex> lock(rwMutex);
    if (!db) return Result::failure("writeBlock: DB not open");

    try {
        leveldb::Status s = db->Put(wo, block.hash, value);
        Result r = fromStatus(s, "writeBlock hash=" + block.hash);
        if (!r) log(2, r.error);
        return r;
    } catch (const std::exception &e) {
        return Result::failure(
            std::string("writeBlock: exception: ") + e.what());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Read single block
// ─────────────────────────────────────────────────────────────────────────────

BlockDB::Result BlockDB::readBlock(const std::string &hash,
                                    Block             &blockOut) noexcept
{
    if (!isValidHash(hash))
        return Result::failure(
            "readBlock: hash must be exactly 64 lowercase hex chars");

    std::shared_lock<std::shared_mutex> lock(rwMutex);
    if (!db) return Result::failure("readBlock: DB not open");

    std::string value;
    try {
        leveldb::Status s = db->Get(leveldb::ReadOptions(), hash, &value);
        if (s.IsNotFound()) return Result::failure("NOT_FOUND");
        if (!s.ok())        return fromStatus(s, "readBlock hash=" + hash);
    } catch (const std::exception &e) {
        return Result::failure(
            std::string("readBlock: exception during get: ") + e.what());
    }

    try {
        blockOut.deserialize(value);
    } catch (const std::exception &e) {
        return Result::failure(
            "readBlock: deserialize exception for hash "
            + hash + ": " + e.what());
    }

    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// Has block
// ─────────────────────────────────────────────────────────────────────────────

bool BlockDB::hasBlock(const std::string &hash) noexcept
{
    if (!isValidHash(hash)) return false;

    std::shared_lock<std::shared_mutex> lock(rwMutex);
    if (!db) return false;

    try {
        std::string value;
        return db->Get(leveldb::ReadOptions(), hash, &value).ok();
    } catch (...) {
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Atomic batch write
// ─────────────────────────────────────────────────────────────────────────────

BlockDB::Result BlockDB::writeBatch(const std::vector<Block> &blocks) noexcept
{
    if (blocks.empty()) return Result::success();

    leveldb::WriteBatch batch;

    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block &b = blocks[i];
        if (!isValidHash(b.hash))
            return Result::failure(
                "writeBatch: invalid hash at index " + std::to_string(i)
                + " — must be 64 lowercase hex chars");

        std::string value;
        try {
            value = b.serialize();
        } catch (const std::exception &e) {
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

    std::unique_lock<std::shared_mutex> lock(rwMutex);
    if (!db) return Result::failure("writeBatch: DB not open");

    try {
        leveldb::Status s = db->Write(wo, &batch);
        Result r = fromStatus(s, "writeBatch ("
                                 + std::to_string(blocks.size()) + " blocks)");
        if (!r) log(2, r.error);
        return r;
    } catch (const std::exception &e) {
        return Result::failure(
            std::string("writeBatch: exception: ") + e.what());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Safe iterator
// ─────────────────────────────────────────────────────────────────────────────

BlockDB::IteratorPtr BlockDB::newIterator() noexcept
{
    std::shared_lock<std::shared_mutex> lock(rwMutex);
    if (!db) return nullptr;
    try {
        return IteratorPtr(db->NewIterator(leveldb::ReadOptions()));
    } catch (...) {
        return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Streaming scan
// ─────────────────────────────────────────────────────────────────────────────

int64_t BlockDB::scan(const ScanCallback &callback) noexcept
{
    if (!callback) {
        log(2, "scan: null callback rejected");
        return -1;
    }

    std::shared_lock<std::shared_mutex> lock(rwMutex);
    if (!db) { log(2, "scan: DB not open"); return -1; }

    int64_t count = 0;
    try {
        IteratorPtr it(db->NewIterator(leveldb::ReadOptions()));
        if (!it) { log(2, "scan: failed to create iterator"); return -1; }

        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            ++count;
            if (!callback(it->key().ToString(), it->value().ToString()))
                break;
        }

        if (!it->status().ok()) {
            log(2, "scan: iterator error after "
                   + std::to_string(count) + " entries: "
                   + it->status().ToString());
            return -1;
        }
    } catch (const std::exception &e) {
        log(2, std::string("scan: exception: ") + e.what());
        return -1;
    }

    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Snapshot — RAII guard eliminates any possibility of a leaked snapshot
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<BlockDB::SnapshotGuard> BlockDB::acquireSnapshot() noexcept
{
    std::shared_lock<std::shared_mutex> lock(rwMutex);
    if (!db) return nullptr;
    try {
        const leveldb::Snapshot *snap = db->GetSnapshot();
        return std::make_unique<SnapshotGuard>(db, snap);
    } catch (...) {
        return nullptr;
    }
}

BlockDB::Result BlockDB::readWithSnapshot(const std::string   &hash,
                                           Block               &blockOut,
                                           const SnapshotGuard &guard) noexcept
{
    if (!isValidHash(hash))
        return Result::failure(
            "readWithSnapshot: hash must be 64 lowercase hex chars");
    if (!guard.valid())
        return Result::failure("readWithSnapshot: invalid snapshot guard");

    std::shared_lock<std::shared_mutex> lock(rwMutex);
    if (!db) return Result::failure("readWithSnapshot: DB not open");

    leveldb::ReadOptions ro;
    ro.snapshot = guard.get();

    std::string value;
    try {
        leveldb::Status s = db->Get(ro, hash, &value);
        if (s.IsNotFound()) return Result::failure("NOT_FOUND");
        if (!s.ok())        return fromStatus(s, "readWithSnapshot hash=" + hash);
    } catch (const std::exception &e) {
        return Result::failure(
            std::string("readWithSnapshot: exception: ") + e.what());
    }

    try {
        blockOut.deserialize(value);
    } catch (const std::exception &e) {
        return Result::failure(
            "readWithSnapshot: deserialize exception: "
            + std::string(e.what()));
    }

    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// Compact
// ─────────────────────────────────────────────────────────────────────────────

void BlockDB::compact() noexcept
{
    std::unique_lock<std::shared_mutex> lock(rwMutex);
    if (!db) { log(1, "compact: DB not open"); return; }
    try {
        db->CompactRange(nullptr, nullptr);
        log(0, "compaction complete");
    } catch (const std::exception &e) {
        log(2, std::string("compact: exception: ") + e.what());
    }
}
