#ifndef EVM_EXECUTOR_H
#define EVM_EXECUTOR_H

#include <evmc/evmc.hpp>
#include <vector>
#include <cstdint>

/**
 * @class MedorEVM
 * @brief Interfaces the MedorCoin network with the evmone execution engine.
 */
class MedorEVM {
public:
    MedorEVM();
    ~MedorEVM();

    /**
     * @brief Executes smart contract bytecode.
     * @param code The Solidity bytecode to run.
     * @param input The transaction data/parameters.
     * @param gas_limit Maximum gas allowed for the execution.
     * @return std::vector<uint8_t> The output/result of the contract call.
     */
    std::vector<uint8_t> execute_transaction(
        const std::vector<uint8_t>& code, 
        const std::vector<uint8_t>& input,
        int64_t gas_limit
    );

private:
    evmc_vm* vm;
};

#endif // EVM_EXECUTOR_H
