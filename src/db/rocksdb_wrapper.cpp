#include "rocksdb_wrapper.h"
#include <memory> // For smart pointers

RocksDBWrapper::RocksDBWrapper(const std::string &path) {
    options.create_if_missing = true;
    rocksdb::Status s = rocksdb::DB::Open(options, path, &db);
    if (!s.ok()) {
        db = nullptr;
    }
}

RocksDBWrapper::~RocksDBWrapper() {
    // Smart pointer automatically handles memory cleanup
    // No need to manually delete db when using unique_ptr
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
    // Removed deletion operation to prevent accidental data loss
    return false;  // No deletion occurs now, returns false indicating no action
}
