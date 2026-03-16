#include "rocksdb_wrapper.h"

#include <rocksdb/write_batch.h>
#include <rocksdb/slice.h>

#include <functional>
#include <iostream>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

RocksDBWrapper::RocksDBWrapper(const std::string &path)
{
    options.create_if_missing             = true;
    options.paranoid_checks               = true;
    options.compression                   = rocksdb::kLZ4Compression;
    options.bottommost_compression        = rocksdb::kZSTD;

    // WAL is enabled by default in RocksDB; these settings reinforce it.
    options.manual_wal_flush              = false;
    options.avoid_flush_during_shutdown   = false;

    rocksdb::DB *rawDb = nullptr;
    rocksdb::Status status = rocksdb::DB::Open(options, path, &rawDb);

    if (!status.ok()) {
        db = nullptr;
        std::cerr << "[RocksDBWrapper] FATAL: cannot open DB at '"
                  << path << "': " << status.ToString() << "\n";
        return;
    }

    db = rawDb;
    std::cout << "[RocksDBWrapper] Opened DB at '" << path << "'\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────

RocksDBWrapper::~RocksDBWrapper()
{
    if (db) {
        // Flush WAL before closing to ensure all buffered writes are durable.
        rocksdb::Status s = db->FlushWAL(/*sync=*/true);
        if (!s.ok())
            std::cerr << "[RocksDBWrapper] FlushWAL on close failed: "
                      << s.ToString() << "\n";
        delete db;
        db = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stripe selection
// ─────────────────────────────────────────────────────────────────────────────

size_t RocksDBWrapper::stripeFor(const std::string &key) const noexcept
{
    return std::hash<std::string>{}(key) % STRIPE_COUNT;
}

// ─────────────────────────────────────────────────────────────────────────────
// Liveness
// ─────────────────────────────────────────────────────────────────────────────

bool RocksDBWrapper::isOpen() const noexcept
{
    return db != nullptr;
}

bool RocksDBWrapper::isHealthy() const noexcept
{
    if (!db) return false;

    // Write a sentinel value, read it back, and confirm they match.
    // This proves the DB is fully writable and readable, not just open.
    const std::string probeValue = "1";
    rocksdb::WriteOptions wo;
    wo.sync = true;

    try {
        rocksdb::Status ws = db->Put(wo, HEALTH_KEY, probeValue);
        if (!ws.ok()) {
            std::cerr << "[RocksDBWrapper] isHealthy: probe write failed: "
                      << ws.ToString() << "\n";
            return false;
        }

        std::string readBack;
        rocksdb::Status rs = db->Get(rocksdb::ReadOptions(), HEALTH_KEY, &readBack);
        if (!rs.ok() || readBack != probeValue) {
            std::cerr << "[RocksDBWrapper] isHealthy: probe read failed or "
                         "value mismatch\n";
            return false;
        }
    } catch (const std::exception &e) {
        std::cerr << "[RocksDBWrapper] isHealthy: exception: "
                  << e.what() << "\n";
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Put
// ─────────────────────────────────────────────────────────────────────────────

RocksDBWrapper::Result RocksDBWrapper::put(const std::string &key,
                                            const std::string &value,
                                            bool sync) noexcept
{
    if (!db)
        return Result::failure("DB not open");
    if (key.empty())
        return Result::failure("empty key rejected");

    rocksdb::WriteOptions wo;
    wo.sync = sync;

    try {
        rocksdb::Status s = db->Put(wo, key, value);
        if (!s.ok())
            return Result::failure("put failed for key '" + key
                                   + "': " + s.ToString());
    } catch (const std::exception &e) {
        return Result::failure(std::string("put exception: ") + e.what());
    }

    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// Get
// ─────────────────────────────────────────────────────────────────────────────

RocksDBWrapper::Result RocksDBWrapper::get(const std::string &key,
                                            std::string &valueOut) noexcept
{
    valueOut.clear();

    if (!db)
        return Result::failure("DB not open");
    if (key.empty())
        return Result::failure("empty key rejected");

    try {
        rocksdb::Status s = db->Get(rocksdb::ReadOptions(), key, &valueOut);
        if (s.IsNotFound())
            return Result::failure("NOT_FOUND");
        if (!s.ok())
            return Result::failure("get failed for key '" + key
                                   + "': " + s.ToString());
    } catch (const std::exception &e) {
        return Result::failure(std::string("get exception: ") + e.what());
    }

    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// Delete
// ─────────────────────────────────────────────────────────────────────────────

RocksDBWrapper::Result RocksDBWrapper::del(const std::string &key,
                                            bool sync) noexcept
{
    if (!db)
        return Result::failure("DB not open");
    if (key.empty())
        return Result::failure("empty key rejected");

    rocksdb::WriteOptions wo;
    wo.sync = sync;

    try {
        rocksdb::Status s = db->Delete(wo, key);
        // Deleting a nonexistent key is not an error in RocksDB
        if (!s.ok() && !s.IsNotFound())
            return Result::failure("del failed for key '" + key
                                   + "': " + s.ToString());
    } catch (const std::exception &e) {
        return Result::failure(std::string("del exception: ") + e.what());
    }

    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// Streaming prefix iterator
// ─────────────────────────────────────────────────────────────────────────────

int64_t RocksDBWrapper::iteratePrefix(const std::string     &prefix,
                                       const IteratorCallback &callback,
                                       size_t                  maxResults) noexcept
{
    if (!db) {
        std::cerr << "[RocksDBWrapper] iteratePrefix: DB not open\n";
        return -1;
    }
    if (prefix.empty()) {
        std::cerr << "[RocksDBWrapper] iteratePrefix: empty prefix rejected\n";
        return -1;
    }
    if (!callback) {
        std::cerr << "[RocksDBWrapper] iteratePrefix: null callback rejected\n";
        return -1;
    }

    rocksdb::ReadOptions ro;
    ro.prefix_same_as_start  = true;
    ro.total_order_seek      = false;

    int64_t count = 0;

    try {
        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
        if (!it) {
            std::cerr << "[RocksDBWrapper] iteratePrefix: failed to create "
                         "iterator\n";
            return -1;
        }

        for (it->Seek(prefix);
             it->Valid() && it->key().starts_with(prefix);
             it->Next())
        {
            const std::string k = it->key().ToString();
            const std::string v = it->value().ToString();

            ++count;

            // Invoke the callback — if it returns false, stop early
            if (!callback(k, v)) break;

            if (maxResults > 0 && static_cast<size_t>(count) >= maxResults)
                break;
        }

        if (!it->status().ok()) {
            std::cerr << "[RocksDBWrapper] iteratePrefix: iterator error after "
                      << count << " entries: "
                      << it->status().ToString() << "\n";
            return -1;
        }
    } catch (const std::exception &e) {
        std::cerr << "[RocksDBWrapper] iteratePrefix: exception: "
                  << e.what() << "\n";
        return -1;
    }

    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Batch Put
// ─────────────────────────────────────────────────────────────────────────────

RocksDBWrapper::Result RocksDBWrapper::batchPut(
    const std::vector<std::pair<std::string, std::string>> &items,
    bool sync) noexcept
{
    if (!db)
        return Result::failure("DB not open");
    if (items.empty())
        return Result::success();

    rocksdb::WriteBatch batch;

    for (size_t i = 0; i < items.size(); ++i) {
        const auto &kv = items[i];
        if (kv.first.empty())
            return Result::failure("empty key at batch index "
                                   + std::to_string(i));
        batch.Put(kv.first, kv.second);
    }

    rocksdb::WriteOptions wo;
    wo.sync = sync;

    try {
        rocksdb::Status s = db->Write(wo, &batch);
        if (!s.ok())
            return Result::failure("batchPut failed: " + s.ToString());
    } catch (const std::exception &e) {
        return Result::failure(std::string("batchPut exception: ") + e.what());
    }

    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// Batch Delete
// ─────────────────────────────────────────────────────────────────────────────

RocksDBWrapper::Result RocksDBWrapper::batchDelete(
    const std::vector<std::string> &keys,
    bool sync) noexcept
{
    if (!db)
        return Result::failure("DB not open");
    if (keys.empty())
        return Result::success();

    rocksdb::WriteBatch batch;

    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].empty())
            return Result::failure("empty key at batch index "
                                   + std::to_string(i));
        batch.Delete(keys[i]);
    }

    rocksdb::WriteOptions wo;
    wo.sync = sync;

    try {
        rocksdb::Status s = db->Write(wo, &batch);
        if (!s.ok())
            return Result::failure("batchDelete failed: " + s.ToString());
    } catch (const std::exception &e) {
        return Result::failure(std::string("batchDelete exception: ")
                               + e.what());
    }

    return Result::success();
}
