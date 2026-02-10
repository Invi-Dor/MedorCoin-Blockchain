#include "storage.h"
#include <string>
#include <vector>

EVMStorage::EVMStorage(const std::string &dbPath) 
    : rocksdb(dbPath) {}

bool EVMStorage::putContractCode(const std::string &addr,
                                 const std::vector<uint8_t> &code) {
    // Contract code stored as raw bytes string
    std::string value(code.begin(), code.end());
    return rocksdb.put("code:" + addr, value);
}

std::vector<uint8_t> EVMStorage::getContractCode(const std::string &addr) {
    std::string value;
    if (!rocksdb.get("code:" + addr, value)) {
        return {};
    }
    return std::vector<uint8_t>(value.begin(), value.end());
}

bool EVMStorage::putContractStorage(const std::string &addr,
                                    const std::string &key,
                                    const std::string &value) {
    return rocksdb.put("storage:" + addr + ":" + key, value);
}

std::string EVMStorage::getContractStorage(const std::string &addr,
                                           const std::string &key) {
    std::string value;
    if (!rocksdb.get("storage:" + addr + ":" + key, value)) {
        return "";
    }
    return value;
}
