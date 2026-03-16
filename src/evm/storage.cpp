#include “storage.h”
#include <stdexcept>
#include <mutex>

static std::mutex storageMutex;

EVMStorage::EVMStorage(const std::string &dbPath)
: rocksdb(dbPath)
{
}

bool EVMStorage::putContractCode(const std::string &addr,
const std::vector<uint8_t> &code)
{
if (addr.empty()) return false;
std::lock_guard<std::mutex> lock(storageMutex);
// Store raw bytes directly — key prefixed to avoid collision
rocksdb::WriteOptions wo;
wo.sync = true;
std::string value(code.begin(), code.end());
return rocksdb.put(“code:” + addr, value);
}

std::vector<uint8_t> EVMStorage::getContractCode(const std::string &addr)
{
if (addr.empty()) return {};
std::lock_guard<std::mutex> lock(storageMutex);
std::string value;
if (!rocksdb.get(“code:” + addr, value)) {
return {};
}
return std::vector<uint8_t>(value.begin(), value.end());
}

bool EVMStorage::putContractStorage(const std::string &addr,
const std::string &key,
const std::string &value)
{
if (addr.empty() || key.empty()) return false;
std::lock_guard<std::mutex> lock(storageMutex);
return rocksdb.put(“storage:” + addr + “:” + key, value);
}

std::string EVMStorage::getContractStorage(const std::string &addr,
const std::string &key)
{
if (addr.empty() || key.empty()) return “”;
std::lock_guard<std::mutex> lock(storageMutex);
std::string value;
if (!rocksdb.get(“storage:” + addr + “:” + key, value)) {
return “”;
}
return value;
}

bool EVMStorage::deleteContractStorage(const std::string &addr,
const std::string &key)
{
if (addr.empty() || key.empty()) return false;
std::lock_guard<std::mutex> lock(storageMutex);
return rocksdb.del(“storage:” + addr + “:” + key);
}

bool EVMStorage::putBalance(const std::string &addr, uint64_t balance)
{
if (addr.empty()) return false;
std::lock_guard<std::mutex> lock(storageMutex);
std::string val = std::to_string(balance);
return rocksdb.put(“balance:” + addr, val);
}

uint64_t EVMStorage::getBalance(const std::string &addr)
{
if (addr.empty()) return 0ULL;
std::lock_guard<std::mutex> lock(storageMutex);
std::string val;
if (!rocksdb.get(“balance:” + addr, val)) {
return 0ULL;
}
try {
return std::stoull(val);
} catch (…) {
return 0ULL;
}
}
