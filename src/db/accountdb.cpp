#include "accountdb.h"

#include <cctype>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// Logging
// ─────────────────────────────────────────────────────────────────────────────

void AccountDB::log(int level, const std::string &msg) const noexcept
{
    if (logger) {
        // User-supplied logger — swallow any exception it might throw so we
        // never propagate out of a noexcept method due to logging alone.
        try { logger(level, msg); } catch (...) {}
        return;
    }
    if (level >= 2)      std::cerr << "[AccountDB][ERROR] " << msg << "\n";
    else if (level == 1) std::cerr << "[AccountDB][WARN]  " << msg << "\n";
    else                 std::cout << "[AccountDB][INFO]  " << msg << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

AccountDB::Result AccountDB::fromStatus(const rocksdb::Status &s,
                                         const std::string     &ctx) noexcept
{
    if (s.ok())         return Result::success();
    if (s.IsNotFound()) return Result::failure("NOT_FOUND: " + ctx);
    return Result::failure(ctx + ": " + s.ToString());
}

bool AccountDB::isValidKey(const std::string &key) noexcept
{
    if (key.empty() || key.size() > MAX_KEY_LEN) return false;

    // Reject null bytes and ASCII control characters (0x00–0x1F, 0x7F).
    // These are not meaningful in account keys and indicate either a
    // programming error or an injection attempt.
    for (unsigned char c : key) {
        if (c == 0x00 || c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

AccountDB::AccountDB(const std::string &path)
{
    if (path.empty()) {
        log(2, "constructor: empty path rejected");
        return;
    }

    options.create_if_missing           = true;
    options.paranoid_checks             = true;
    options.compression                 = rocksdb::kLZ4Compression;
    options.bottommost_compression      = rocksdb::kZSTD;
    options.manual_wal_flush            = false;
    options.avoid_flush_during_shutdown = false;

    rocksdb::DB    *rawDb = nullptr;
    rocksdb::Status s     = rocksdb::DB::Open(options, path, &rawDb);

    if (!s.ok()) {
        db = nullptr;
        log(2, "constructor: cannot open DB at '" + path
               + "': " + s.ToString());
        return;
    }

    db = rawDb;
    log(0, "Opened at '" + path + "'");
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────

AccountDB::~AccountDB()
{
    if (!db) return;

    // Attempt WAL flush; retry once on transient failure before warning.
    rocksdb::Status s = db->FlushWAL(/*sync=*/true);
    if (!s.ok()) {
        log(1, "destructor: FlushWAL failed on first attempt: "
               + s.ToString() + " — retrying");
        s = db->FlushWAL(true);
        if (!s.ok())
            log(1, "destructor: FlushWAL retry also failed: "
                   + s.ToString()
                   + ". Some buffered writes may not be durable.");
    }

    delete db;
    db = nullptr;
    log(0, "Closed");
}

// ─────────────────────────────────────────────────────────────────────────────
// Liveness
// ─────────────────────────────────────────────────────────────────────────────

bool AccountDB::isOpen() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(dbMutex);
    return db != nullptr;
}

bool AccountDB::isHealthy() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return false;

    rocksdb::WriteOptions wo;
    wo.sync = true;

    // Write a sentinel value then read it back to confirm full read/write
    // capability — not just that the handle is non-null.
    try {
        rocksdb::Status ws = db->Put(wo, HEALTH_KEY, "1");
        if (!ws.ok()) {
            log(2, "isHealthy: probe write failed: " + ws.ToString());
            return false;
        }

        std::string readBack;
        rocksdb::Status rs =
            db->Get(rocksdb::ReadOptions(), HEALTH_KEY, &readBack);
        if (!rs.ok() || readBack != "1") {
            log(2, "isHealthy: probe read failed or value mismatch");
            return false;
        }
    } catch (...) {
        // Documented trade-off: catching all exceptions here is intentional.
        // Any exception from the underlying DB at this point indicates a
        // severe infrastructure failure; returning false is the correct
        // safe response.
        log(2, "isHealthy: unexpected exception during probe");
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Put
// ─────────────────────────────────────────────────────────────────────────────

AccountDB::Result AccountDB::put(const std::string &key,
                                  const std::string &value,
                                  bool sync) noexcept
{
    if (!isValidKey(key))
        return Result::failure(
            "put: invalid key — must be 1–" + std::to_string(MAX_KEY_LEN)
            + " printable characters");

    rocksdb::WriteOptions wo;
    wo.sync = sync;

    std::unique_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return Result::failure("put: DB not open");

    try {
        rocksdb::Status s = db->Put(wo, key, value);
        Result r = fromStatus(s, "put key='" + key + "'");
        if (!r) log(2, r.error);
        return r;
    } catch (...) {
        return Result::failure("put: unexpected exception for key='" + key + "'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Get
// ─────────────────────────────────────────────────────────────────────────────

AccountDB::Result AccountDB::get(const std::string &key,
                                  std::string       &valueOut) noexcept
{
    valueOut.clear();

    if (!isValidKey(key))
        return Result::failure("get: invalid key");

    std::shared_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return Result::failure("get: DB not open");

    try {
        rocksdb::Status s =
            db->Get(rocksdb::ReadOptions(), key, &valueOut);
        if (s.IsNotFound()) return Result::failure("NOT_FOUND");
        if (!s.ok()) {
            Result r = fromStatus(s, "get key='" + key + "'");
            log(2, r.error);
            return r;
        }
        return Result::success();
    } catch (...) {
        return Result::failure("get: unexpected exception for key='" + key + "'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Delete
// ─────────────────────────────────────────────────────────────────────────────

AccountDB::Result AccountDB::del(const std::string &key, bool sync) noexcept
{
    if (!isValidKey(key))
        return Result::failure("del: invalid key");

    rocksdb::WriteOptions wo;
    wo.sync = sync;

    std::unique_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return Result::failure("del: DB not open");

    try {
        rocksdb::Status s = db->Delete(wo, key);
        // Deleting a nonexistent key is not an error in RocksDB.
        if (!s.ok() && !s.IsNotFound()) {
            Result r = fromStatus(s, "del key='" + key + "'");
            log(2, r.error);
            return r;
        }
        return Result::success();
    } catch (...) {
        return Result::failure("del: unexpected exception for key='" + key + "'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Atomic batch write
//
// The exclusive lock covers all keys in the batch for its full duration.
// RocksDB WriteBatch is atomic at the engine level, so readers will either
// see all writes or none — there is no partial state visible to other threads.
// ─────────────────────────────────────────────────────────────────────────────

AccountDB::Result AccountDB::writeBatch(
    const std::vector<std::pair<std::string, std::string>> &items,
    bool sync) noexcept
{
    if (items.empty()) return Result::success();

    // Validate everything before touching the DB
    for (size_t i = 0; i < items.size(); ++i) {
        if (!isValidKey(items[i].first))
            return Result::failure(
                "writeBatch: invalid key at index " + std::to_string(i)
                + " — must be 1–" + std::to_string(MAX_KEY_LEN)
                + " printable characters");
    }

    rocksdb::WriteBatch batch;
    for (const auto &kv : items)
        batch.Put(kv.first, kv.second);

    rocksdb::WriteOptions wo;
    wo.sync = sync;

    std::unique_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return Result::failure("writeBatch: DB not open");

    try {
        rocksdb::Status s = db->Write(wo, &batch);
        Result r = fromStatus(s, "writeBatch ("
                                 + std::to_string(items.size()) + " items)");
        if (!r) log(2, r.error);
        return r;
    } catch (...) {
        return Result::failure("writeBatch: unexpected exception");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Atomic batch delete
// ─────────────────────────────────────────────────────────────────────────────

AccountDB::Result AccountDB::deleteBatch(
    const std::vector<std::string> &keys,
    bool sync) noexcept
{
    if (keys.empty()) return Result::success();

    for (size_t i = 0; i < keys.size(); ++i) {
        if (!isValidKey(keys[i]))
            return Result::failure(
                "deleteBatch: invalid key at index " + std::to_string(i));
    }

    rocksdb::WriteBatch batch;
    for (const auto &k : keys)
        batch.Delete(k);

    rocksdb::WriteOptions wo;
    wo.sync = sync;

    std::unique_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return Result::failure("deleteBatch: DB not open");

    try {
        rocksdb::Status s = db->Write(wo, &batch);
        Result r = fromStatus(s, "deleteBatch ("
                                 + std::to_string(keys.size()) + " keys)");
        if (!r) log(2, r.error);
        return r;
    } catch (...) {
        return Result::failure("deleteBatch: unexpected exception");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Streaming prefix scan
//
// A shared lock is held for the full duration of the scan. This prevents
// any writer from modifying keys the iterator has not yet visited, which
// eliminates the concurrency nuance that prefix-stripe locking introduces.
// ─────────────────────────────────────────────────────────────────────────────

int64_t AccountDB::iteratePrefix(const std::string  &prefix,
                                  const ScanCallback &callback,
                                  size_t              maxResults) noexcept
{
    if (prefix.empty()) {
        log(2, "iteratePrefix: empty prefix rejected");
        return -1;
    }
    if (!callback) {
        log(2, "iteratePrefix: null callback rejected");
        return -1;
    }

    std::shared_lock<std::shared_mutex> lock(dbMutex);
    if (!db) { log(2, "iteratePrefix: DB not open"); return -1; }

    rocksdb::ReadOptions ro;
    ro.prefix_same_as_start = true;
    ro.total_order_seek     = false;

    int64_t count = 0;
    try {
        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
        if (!it) {
            log(2, "iteratePrefix: failed to create iterator");
            return -1;
        }

        for (it->Seek(prefix);
             it->Valid() && it->key().starts_with(prefix);
             it->Next())
        {
            ++count;
            if (!callback(it->key().ToString(), it->value().ToString()))
                break;
            if (maxResults > 0 && static_cast<size_t>(count) >= maxResults)
                break;
        }

        if (!it->status().ok()) {
            log(2, "iteratePrefix: iterator error after "
                   + std::to_string(count) + " entries: "
                   + it->status().ToString());
            return -1;
        }
    } catch (...) {
        // Documented trade-off: any exception here indicates a severe
        // infrastructure failure. Returning -1 signals the error to the
        // caller without propagating out of a noexcept boundary.
        log(2, "iteratePrefix: unexpected exception");
        return -1;
    }

    return count;
}
