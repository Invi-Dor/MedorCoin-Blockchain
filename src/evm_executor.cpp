#include <evmc/evmc.hpp>
#include <evmone/evmone.h>
#include <vector>
#include <iostream>

/**
 * @file evm_executor.cpp
 * @brief Logic to execute Solidity Bytecode on the MedorCoin Network using evmone.
 */

class MedorEVM {
public:
    MedorEVM() {
        // Load the evmone engine
        vm = evmc_create_evmone();
    }

    ~MedorEVM() {
        vm->destroy(vm);
    }

    // Executes a transaction on the MedorCoin network
    std::vector<uint8_t> execute_transaction(
        const std::vector<uint8_t>& code, 
        const std::vector<uint8_t>& input,
        int64_t gas_limit
    ) {
        evmc_message msg = {};
        msg.kind = EVMC_CALL;
        msg.gas = gas_limit;
        msg.input_data = input.data();
        msg.input_size = input.size();

        // Run the code
        evmc_result result = vm->execute(vm, nullptr, EVMC_MAX_REVISION, &msg, code.data(), code.size());

        std::vector<uint8_t> output;
        if (result.status_code == EVMC_SUCCESS) {
            output.assign(result.output_data, result.output_data + result.output_size);
        } else {
            std::cerr << "EVM Execution Failed with status: " << result.status_code << std::endl;
        }

        // Clean up result
        if (result.release) result.release(&result);
        return output;
    }

private:
    evmc_vm* vm;
};

// Example entry point for testing the integration
int main() {
    MedorEVM medor_evm;
    std::cout << "MedorCoin EVM Engine Initialized Successfully." << std::endl;
    return 0;
}
