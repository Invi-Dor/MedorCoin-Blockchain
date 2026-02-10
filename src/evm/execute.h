#ifndef MEDOR_EVM_EXECUTE_H
#define MEDOR_EVM_EXECUTE_H

#include <vector>
#include <cstdint>
#include <evmc/evmc.hpp>

class EVMStorage;

// Executes EVM code using persistent storage
class EVMExecutor
{
public:
    static evmc_result executeContract(
        EVMStorage &storage,
        const std::vector<uint8_t> &bytecode,
        const std::vector<uint8_t> &inputData,
        uint64_t gasLimit,
        evmc_address to,
        evmc_address from
    );
};

#endif // MEDOR_EVM_EXECUTE_H
