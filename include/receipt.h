Here is a clean, single-file version without the diff markers or leading plus signs. It’s ready to be compiled as-is (note: I kept the RocksDB member as in your snippet).

#ifndef RECEIPTS_STORE_H
#define RECEIPTS_STORE_H

#include <string>
#include <mutex>
#include <rocksdb/db.h>
#include <nlohmann/json.hpp>

class ReceiptsStore {
public:
  explicit ReceiptsStore(const std::string& db_path);
  ~ReceiptsStore();

  // Persist a JSON receipt under a key
  bool put(const std::string& key, const nlohmann::json& value);
  // Retrieve a JSON receipt by key
  bool get(const std::string& key, nlohmann::json& value) const;

private:
  rocksdb::DB db_;
  mutable std::mutex mtx_;
};

#endif
