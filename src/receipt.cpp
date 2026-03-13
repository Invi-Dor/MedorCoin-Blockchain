+++ b/receipts_store.cpp
+ #include "receipts_store.h"
+ #include <iostream>
+ #include <rocksdb/options.h>
+
+ using json = nlohmann::json;
+
+ ReceiptsStore::ReceiptsStore(const std::string& db_path) : db_(nullptr) {
+   rocksdb::Options options;
+   options.create_if_missing = true;
+   auto status = rocksdb::DB::Open(options, db_path, &db_);
+   if (!status.ok()) {
+     std::cerr << "[ReceiptsStore] Failed to open RocksDB at " << db_path
+               << ": " << status.ToString() << std::endl;
+     db_ = nullptr;
+   }
+ }
+
+ ReceiptsStore::~ReceiptsStore() {
+   delete db_;
+ }
+
+ bool ReceiptsStore::put(const std::string& key, const json& value) {
+   if (!db_) return false;
+   std::lock_guard<std::mutex> lock(mtx_);
+   std::string s = value.dump();
+   auto status = db_->Put(rocksdb::WriteOptions(), key, s);
+   return status.ok();
+ }
+
+ bool ReceiptsStore::get(const std::string& key, json& value) const {
+   if (!db_) return false;
+   std::lock_guard<std::mutex> lock(mtx_);
+   std::string s;
+   auto status = db_->Get(rocksdb::ReadOptions(), key, &s);
+   if (!status.ok()) return false;
+   try {
+     value = json::parse(s);
+   } catch (...) {
+     return false;
+   }
+   return true;
+ }
