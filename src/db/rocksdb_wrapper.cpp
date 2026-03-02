#include "rocksdb_wrapper.h"
#include <rocksdb/write_batch.h>

// -----------------------
// Constructor / Destructor
// -----------------------

RocksDBWrapper::RocksDBWrapper(const std::string &path) {
    options.create_if_missing = true;
    rocksdb::Status status = rocksdb::DB::Open(options, path, &db);
    if (!status.ok()) {
        db = nullptr;
    }
}

RocksDBWrapper::~RocksDBWrapper() {
    if (db) {
        delete db;
        db = nullptr;
    }
}

// -----------------------
// Basic Put / Get / Delete
// -----------------------

bool RocksDBWrapper::put(const std::string &key, const std::string &value) {
    if (!db) return false;
    rocksdb::Status s = db->Put(rocksdb::WriteOptions(), key, value);
    return s.ok();
}

bool RocksDBWrapper::get(const std::string &key, std::string &value) {
    if (!db) return false;
    rocksdb::Status s = db->Get(rocksdb::ReadOptions(), key, &value);
    return s.ok();
}

bool RocksDBWrapper::del(const std::string &key) {
    if (!db) return false;
    rocksdb::Status s = db->Delete(rocksdb::WriteOptions(), key);
    return s.ok();
}

// -----------------------
// Prefix Iteration
// -----------------------

std::vector<std::pair<std::string, std::string>>
RocksDBWrapper::iteratePrefix(const std::string &prefix) {
    std::vector<std::pair<std::string, std::string>> result;
    if (!db) return result;

    std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions()));
    for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
        result.emplace_back(it->key().ToString(), it->value().ToString());
    }
    return result;
}

// -----------------------
// Batch Operations
// -----------------------

bool RocksDBWrapper::batchPut(
    const std::vector<std::pair<std::string, std::string>> &items
) {
    if (!db) return false;
    rocksdb::WriteBatch batch;
    for (const auto &kv : items) {
        batch.Put(kv.first, kv.second);
    }
    rocksdb::Status s = db->Write(rocksdb::WriteOptions(), &batch);
    return s.ok();
}

bool RocksDBWrapper::batchDelete(const std::vector<std::string> &keys) {
    if (!db) return false;
    rocksdb::WriteBatch batch;
    for (const auto &key : keys) {
        batch.Delete(key);
    }
    rocksdb::Status s = db->Write(rocksdb::WriteOptions(), &batch);
    return s.ok();
}
