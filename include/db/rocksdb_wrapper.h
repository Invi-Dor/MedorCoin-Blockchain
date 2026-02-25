#ifndef ROCKSDB_WRAPPER_H
#define ROCKSDB_WRAPPER_H

#include <rocksdb/db.h>
#include <string>

class RocksDBWrapper {
private:
    rocksdb::DB* db; // Pointer to the RocksDB instance
    rocksdb::Options options; // RocksDB options

public:
    // Constructor that opens the database at the given path
    explicit RocksDBWrapper(const std::string &path);

    // Destructor that automatically handles cleanup
    ~RocksDBWrapper();

    // Method to put a key-value pair into the database
    bool put(const std::string &key, const std::string &value);

    // Method to get a value by key from the database
    bool get(const std::string &key, std::string &value);

    // Method to delete a key from the database
    bool del(const std::string &key);
};

#endif // ROCKSDB_WRAPPER_H
