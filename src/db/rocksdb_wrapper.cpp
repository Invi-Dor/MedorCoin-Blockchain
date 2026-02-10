#include "rocksdb_wrapper.h"

RocksDBWrapper::RocksDBWrapper(const std::string &path) {
    options.create_if_missing = true;
    rocksdb::Status s = rocksdb::DB::Open(options, path, &db);
    if (!s.ok()) {
        db = nullptr;
    }
}

RocksDBWrapper::~RocksDBWrapper() {
    if (db) {
        delete db;
    }
}

bool RocksDBWrapper::put(const std::string &key, const std::string &value) {
    if (!db) return false;
    rocksdb::Status s =
        db->Put(rocksdb::WriteOptions(), key, value);
    return s.ok();
}

bool RocksDBWrapper::get(const std::string &key, std::string &value) {
    if (!db) return false;
    rocksdb::Status s =
        db->Get(rocksdb::ReadOptions(), key, &value);
    return s.ok();
}

bool RocksDBWrapper::del(const std::string &key) {
    if (!db) return false;
    rocksdb::Status s =
        db->Delete(rocksdb::WriteOptions(), key);
    return s.ok();
}
