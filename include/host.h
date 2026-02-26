#ifndef MEDOR_EVM_HOST_H
#define MEDOR_EVM_HOST_H

#include <evmc/evmc.hpp>
#include <string>
#include <vector>
#include "evm/storage.h"

// EVMC Host interface: This tells the EVM how to read/write blockchain state
class MedorEVMHost : public evmc::Host
{
public:
    // Constructor takes a reference to persistent RocksDB contract storage
    MedorEVMHost(EVMStorage &storage);

    // Read a storage slot for a contract
    evmc_bytes32 get_storage(evmc::address addr, evmc_bytes32 key) override;

    // Write a storage slot for a contract
    evmc_storage_status set_storage(
        evmc::address addr,
        evmc_bytes32 key,
        evmc_bytes32 val) override;

    // Get raw contract code (bytecode) for an address
    std::vector<uint8_t> get_code(evmc::address addr) override;

    // Get the account balance (native token) for an address
    uint64_t get_balance(evmc::address addr) override;

    // Emit log event for a contract
    void emit_log(evmc::address addr,
                  const uint8_t *data,
                  size_t data_size,
                  const evmc::bytes32 topics[],
                  size_t topics_count) override;

private:
    EVMStorage &storageDB;
};

#endif // MEDOR_EVM_HOST_H
