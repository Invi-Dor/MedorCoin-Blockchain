#ifndef ROCKSDB_WRAPPER_H
#define ROCKSDB_WRAPPER_H

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <string>
#include <vector>
#include <utility>

class RocksDBWrapper {
private:
    rocksdb::DB* db;
    rocksdb::Options options;

public:
    // Constructor: open (or create) the database at the given path
    explicit RocksDBWrapper(const std::string &path);

    // Destructor: close the database
    ~RocksDBWrapper();

    // Basic operations
    bool put(const std::string &key, const std::string &value);
    bool get(const std::string &key, std::string &value);
    bool del(const std::string &key);

    // Iteration: fetch all key/value pairs whose keys begin with the prefix
    std::vector<std::pair<std::string, std::string>> iteratePrefix(const std::string &prefix);

    // Batch operations
    bool batchPut(const std::vector<std::pair<std::string, std::string>> &items);
    bool batchDelete(const std::vector<std::string> &keys);
};

#endif // ROCKSDB_WRAPPER_H
