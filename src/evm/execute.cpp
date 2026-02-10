#include "execute.h"
#include "host.h"
#include "storage.h"

#include <evmone/evmone.h>
#include <iostream>

// --------------------------------------------------
// Execute EVM contract (call or deploy logic outside)
// --------------------------------------------------

evmc_result EVMExecutor::executeContract(
    EVMStorage& storage,
    const std::vector<uint8_t>& bytecode,
    const std::vector<uint8_t>& inputData,
    uint64_t gasLimit,
    evmc_address to,
    evmc_address from
) {
    // Create EVM instance once
    static evmc::VM vm{ evmc_create_evmone() };

    // Host backed by persistent storage
    MedorEVMHost host(storage);

    // Prepare EVM message
    evmc_message msg{};
    msg.kind = EVMC_CALL;
    msg.depth = 0;
    msg.gas = gasLimit;
    msg.destination = to;
    msg.sender = from;
    msg.input_data = inputData.data();
    msg.input_size = inputData.size();
    msg.value = 0;      // native value transfer (future)
    msg.flags = 0;

    // Execute bytecode
    evmc_result result = vm.execute(
        host,
        EVMC_CANCUN,
        bytecode.data(),
        bytecode.size(),
        &msg
    );

    // Basic error reporting
    if (result.status_code != EVMC_SUCCESS) {
        std::cerr << "[EVM] Execution failed. Status: "
                  << result.status_code << std::endl;
    }

    return result;
}
