#ifndef MEDOR_ROCKSDB_WRAPPER_H
#define MEDOR_ROCKSDB_WRAPPER_H

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <string>

// RocksDB wrapper to store and retrieve state
class RocksDBWrapper {
public:
    RocksDBWrapper(const std::string &path);
    ~RocksDBWrapper();

    bool put(const std::string &key, const std::string &value);
    bool get(const std::string &key, std::string &value);
    bool del(const std::string &key);

private:
    rocksdb::DB *db;
    rocksdb::Options options;
};

#endif // MEDOR_ROCKSDB_WRAPPER_H
