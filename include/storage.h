#ifndef MEDOR_EVM_STORAGE_H
#define MEDOR_EVM_STORAGE_H

#include <string>
#include <vector>
#include "db/rocksdb_wrapper.h"

// Persistent storage for EVM contracts and state
class EVMStorage {
public:
    // dbPath e.g., "rocksdb_evm_state"
    EVMStorage(const std::string &dbPath);

    // Contract bytecode CRUD
    bool putContractCode(const std::string &addr,
                         const std::vector<uint8_t> &code);

    std::vector<uint8_t> getContractCode(const std::string &addr);

    // Contract storage CRUD
    bool putContractStorage(const std::string &addr,
                            const std::string &key,
                            const std::string &value);

    std::string getContractStorage(const std::string &addr,
                                   const std::string &key);

private:
    RocksDBWrapper rocksdb;
};

#endif // MEDOR_EVM_STORAGE_H
