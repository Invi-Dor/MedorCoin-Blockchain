#ifndef MEDOR_ACCOUNTDB_H
#define MEDOR_ACCOUNTDB_H

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <string>

class AccountDB {
public:
    AccountDB(const std::string &path);
    ~AccountDB();

    bool put(const std::string &key, const std::string &val);
    bool get(const std::string &key, std::string &val);
    bool del(const std::string &key);

private:
    rocksdb::DB *db;
    rocksdb::Options options;
};

#endif // MEDOR_ACCOUNTDB_H
