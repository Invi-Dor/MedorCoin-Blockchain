#include "transaction.h"
#include "crypto.h"          // doubleSHA256
#include "evm/execute.h"
#include "evm/storage.h"

#include <vector>
#include <cstdint>
#include <string>
#include <cstring>
#include <iostream>
#include <evmc/evmc.hpp>

// -------------------------------
// Helpers for Serialization
// -------------------------------

static void writeUInt32LE(std::vector<unsigned char>& buf, uint32_t value)
{
    buf.push_back(value & 0xff);
    buf.push_back((value >> 8) & 0xff);
    buf.push_back((value >> 16) & 0xff);
    buf.push_back((value >> 24) & 0xff);
}

static void writeUInt64LE(std::vector<unsigned char>& buf, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        buf.push_back(value & 0xff);
        value >>= 8;
    }
}

// -------------------------------
// Serialize UTXO Transaction
// -------------------------------

static void serializeUTXO(const Transaction& tx, std::vector<unsigned char>& buf)
{
    writeUInt32LE(buf, tx.version);

    buf.push_back(static_cast<unsigned char>(tx.inputs.size()));
    for (auto& in : tx.inputs) {
        for (size_t i = 0; i < 32 && i < in.prevTxHash.size(); ++i)
            buf.push_back(static_cast<unsigned char>(in.prevTxHash[i]));

        writeUInt32LE(buf, in.outputIndex);
        buf.push_back(0); // scriptSig length
        writeUInt32LE(buf, 0xffffffff);
    }

    buf.push_back(static_cast<unsigned char>(tx.outputs.size()));
    for (auto& out : tx.outputs) {
        writeUInt64LE(buf, out.value);
        buf.push_back(0); // scriptPubKey length
    }

    writeUInt32LE(buf, tx.lockTime);
}

// -------------------------------
// Serialize EVM Transaction
// -------------------------------

static void serializeEVM(const Transaction& tx, std::vector<unsigned char>& buf)
{
    writeUInt32LE(buf, tx.version);
    buf.push_back(static_cast<unsigned char>(tx.type));

    buf.insert(buf.end(), tx.fromAddress.begin(), tx.fromAddress.end());
    buf.insert(buf.end(), tx.toAddress.begin(), tx.toAddress.end());

    writeUInt64LE(buf, tx.gasLimit);

    writeUInt32LE(buf, static_cast<uint32_t>(tx.data.size()));
    buf.insert(buf.end(), tx.data.begin(), tx.data.end());
}

// -------------------------------
// Compute Transaction Hash
// -------------------------------

void Transaction::calculateHash()
{
    std::vector<unsigned char> serialized;

    if (type == TxType::Standard)
        serializeUTXO(*this, serialized);
    else
        serializeEVM(*this, serialized);

    txHash = doubleSHA256(std::string(serialized.begin(), serialized.end()));
}

// -------------------------------
// CONTRACT DEPLOY
// -------------------------------

static bool processContractDeploy(const Transaction& tx)
{
    // Persistent EVM state
    EVMStorage storage("rocksdb_evm_state");

    // Deterministic contract address
    std::string seed = tx.fromAddress + tx.txHash;
    std::string contractAddress = doubleSHA256(seed);

    // Store bytecode
    storage.putContractCode(contractAddress, tx.data);

    std::cout << "[EVM] Contract deployed at: "
              << contractAddress << std::endl;

    return true;
}

// -------------------------------
// CONTRACT CALL
// -------------------------------

static bool processContractCall(const Transaction& tx)
{
    EVMStorage storage("rocksdb_evm_state");

    // Load bytecode
    std::vector<uint8_t> bytecode =
        storage.getContractCode(tx.toAddress);

    if (bytecode.empty()) {
        std::cerr << "[EVM] No contract code at address "
                  << tx.toAddress << std::endl;
        return false;
    }

    // Convert addresses
    evmc_address from{};
    evmc_address to{};

    std::memcpy(from.bytes,
                tx.fromAddress.data(),
                std::min(tx.fromAddress.size(), sizeof(from.bytes)));

    std::memcpy(to.bytes,
                tx.toAddress.data(),
                std::min(tx.toAddress.size(), sizeof(to.bytes)));

    // Execute
    evmc_result result = EVMExecutor::executeContract(
        storage,
        bytecode,
        tx.data,
        tx.gasLimit,
        to,
        from
    );

    if (result.status_code != EVMC_SUCCESS) {
        std::cerr << "[EVM] Execution failed, status: "
                  << result.status_code << std::endl;
        return false;
    }

    std::cout << "[EVM] Contract call success" << std::endl;
    return true;
}

// -------------------------------
// TRANSACTION DISPATCH
// -------------------------------

bool processTransaction(const Transaction& tx)
{
    switch (tx.type) {

        case TxType::Standard:
            // UTXO logic handled elsewhere
            std::cout << "[TX] Standard transaction processed\n";
            return true;

        case TxType::ContractDeploy:
            return processContractDeploy(tx);

        case TxType::ContractCall:
            return processContractCall(tx);

        default:
            std::cerr << "[TX] Unknown transaction type\n";
            return false;
    }
}

// -------------------------------
// Debug Print
// -------------------------------

void printTransaction(const Transaction& tx)
{
    std::cout << "Tx Hash: " << tx.txHash << std::endl;
    std::cout << "Type: ";

    switch (tx.type) {
        case TxType::Standard:
            std::cout << "Standard UTXO";
            break;
        case TxType::ContractDeploy:
            std::cout << "Contract Deploy";
            break;
        case TxType::ContractCall:
            std::cout << "Contract Call";
            break;
    }

    std::cout << std::endl;

    if (tx.type == TxType::Standard) {
        std::cout << "Inputs: " << tx.inputs.size()
                  << ", Outputs: " << tx.outputs.size() << std::endl;
    } else {
        std::cout << "From: " << tx.fromAddress
                  << ", To: " << tx.toAddress
                  << ", Gas: " << tx.gasLimit
                  << ", Data size: " << tx.data.size()
                  << std::endl;
    }
}
