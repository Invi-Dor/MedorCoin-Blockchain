#include "accountdb.h"

AccountDB::AccountDB(const std::string &path) {
    options.create_if_missing = true;
    rocksdb::Status s = rocksdb::DB::Open(options, path, &db);
    if (!s.ok()) db = nullptr;
}

AccountDB::~AccountDB() {
    if (db) delete db;
}

bool AccountDB::put(const std::string &key, const std::string &val) {
    if (!db) return false;
    return db->Put(rocksdb::WriteOptions(), key, val).ok();
}

bool AccountDB::get(const std::string &key, std::string &val) {
    if (!db) return false;
    return db->Get(rocksdb::ReadOptions(), key, &val).ok();
}

bool AccountDB::del(const std::string &key) {
    if (!db) return false;
    return db->Delete(rocksdb::WriteOptions(), key).ok();
}
