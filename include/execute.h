#ifndef MEDOR_EVM_EXECUTE_H
#define MEDOR_EVM_EXECUTE_H

#include <vector>
#include <string>
#include <evmc/evmc.hpp>

class EVMStorage;
class Blockchain;

class EVMExecutor {
public:

    /**
     * Execute EVM bytecode using EVMC.
     *
     * @param storage: access to contract storage
     * @param bytecode: contract code to run
     * @param inputData: calldata (msg.data)
     * @param gasLimit: gas budget for this execution
     * @param to: destination (contract address)
     * @param from: sender address
     * @param chain: reference to blockchain state
     * @param minerAddress: miner or executor address (for tip reward)
     *
     * @return an evmc::Result with status_code, gas_left, and potential return data
     */
    static evmc::Result executeContract(
        EVMStorage &storage,
        const std::vector<uint8_t> &bytecode,
        const std::vector<uint8_t> &inputData,
        uint64_t gasLimit,
        const evmc_address &to,
        const evmc_address &from,
        Blockchain &chain,
        const std::string &minerAddress
    );
};

#endif // MEDOR_EVM_EXECUTE_H
