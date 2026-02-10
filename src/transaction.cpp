#include "transaction.h"
#include "crypto.h"          // doubleSHA256
#include "evm/execute.h"
#include "evm/storage.h"
#include "blockchain.h"      // Blockchain access for fees & state
#include <vector>
#include <cstdint>
#include <string>
#include <cstring>
#include <iostream>
#include <evmc/evmc.hpp>

// -------------------------------
// Helpers for Serialization
// -------------------------------

static void writeUInt32LE(std::vector<unsigned char>& buf, uint32_t value) {
    buf.push_back(value & 0xff);
    buf.push_back((value >> 8) & 0xff);
    buf.push_back((value >> 16) & 0xff);
    buf.push_back((value >> 24) & 0xff);
}

static void writeUInt64LE(std::vector<unsigned char>& buf, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(value & 0xff);
        value >>= 8;
    }
}

// -------------------------------
// Serialize UTXO Transaction
// -------------------------------

static void serializeUTXO(const Transaction& tx, std::vector<unsigned char>& buf) {
    writeUInt32LE(buf, tx.version);
    buf.push_back(static_cast<unsigned char>(tx.inputs.size()));

    for (const auto& in : tx.inputs) {
        for (size_t i = 0; i < 32 && i < in.prevTxHash.size(); ++i)
            buf.push_back(static_cast<unsigned char>(in.prevTxHash[i]));
        writeUInt32LE(buf, in.outputIndex);
        buf.push_back(0); // scriptSig length
        writeUInt32LE(buf, 0xffffffff);
    }

    buf.push_back(static_cast<unsigned char>(tx.outputs.size()));
    for (const auto& out : tx.outputs) {
        writeUInt64LE(buf, out.value);
        buf.push_back(0); // scriptPubKey length
    }

    writeUInt32LE(buf, tx.lockTime);
}

// -------------------------------
// Serialize EVM Transaction
// -------------------------------

static void serializeEVM(const Transaction& tx, std::vector<unsigned char>& buf) {
    writeUInt32LE(buf, tx.version);
    buf.push_back(static_cast<unsigned char>(tx.type));
    buf.insert(buf.end(), tx.fromAddress.begin(), tx.fromAddress.end());
    buf.insert(buf.end(), tx.toAddress.begin(), tx.toAddress.end());
    writeUInt64LE(buf, tx.gasLimit);
    writeUInt64LE(buf, tx.maxFeePerGas);
    writeUInt64LE(buf, tx.maxPriorityFeePerGas);

    uint32_t dataLen = static_cast<uint32_t>(tx.data.size());
    writeUInt32LE(buf, dataLen);
    buf.insert(buf.end(), tx.data.begin(), tx.data.end());
}

// -------------------------------
// Compute Transaction Hash
// -------------------------------

void Transaction::calculateHash() {
    std::vector<unsigned char> serialized;

    if (type == TxType::Standard) {
        serializeUTXO(*this, serialized);
    } else {
        serializeEVM(*this, serialized);
    }

    txHash = doubleSHA256(std::string(serialized.begin(), serialized.end()));
}

// -------------------------------
// UTXO Processing
// -------------------------------

static bool processStandardTx(const Transaction& tx, Blockchain& chain) {
    // Placeholder for real UTXO logic
    std::cout << "[TX] Standard UTXO transaction processed: " << tx.txHash << std::endl;
    return true;
}

// -------------------------------
// CONTRACT DEPLOY
// -------------------------------

static bool processContractDeploy(const Transaction& tx, Blockchain& chain, const std::string& minerAddress) {
    EVMStorage storage("rocksdb_evm_state");

    // Deterministic contract address
    std::string seed = tx.fromAddress + tx.txHash;
    std::string contractAddress = doubleSHA256(seed);

    storage.putContractCode(contractAddress, tx.data);

    std::cout << "[EVM] Contract deployed at: " << contractAddress << std::endl;
    return true;
}

// -------------------------------
// CONTRACT CALL + FEE DEDUCTION
// -------------------------------

static bool processContractCall(const Transaction& tx, Blockchain& chain, const std::string& minerAddress) {
    EVMStorage storage("rocksdb_evm_state");

    // Load the contract bytecode
    std::vector<uint8_t> bytecode = storage.getContractCode(tx.toAddress);
    if (bytecode.empty()) {
        std::cerr << "[EVM] Contract not found at: " << tx.toAddress << std::endl;
        return false;
    }

    // Convert sender/to address to evmc_address
    evmc_address from{};
    evmc_address to{};
    std::memcpy(from.bytes, tx.fromAddress.data(), std::min(tx.fromAddress.size(), sizeof(from.bytes)));
    std::memcpy(to.bytes, tx.toAddress.data(), std::min(tx.toAddress.size(), sizeof(to.bytes)));

    // Execute the contract
    evmc_result result = EVMExecutor::executeContract(
        storage,
        bytecode,
        tx.data,
        tx.gasLimit,
        to,
        from,
        chain,
        minerAddress
    );

    // Calculate gas used
    uint64_t gasUsed = tx.gasLimit - result.gas_left;

    // Get base fee from current chain state
    uint64_t baseFee = chain.getCurrentBaseFee();

    // Determine tip and ensure it does not exceed maxFee
    uint64_t tip = tx.maxPriorityFeePerGas;
    uint64_t maxFee = tx.maxFeePerGas;
    uint64_t effPrice = std::min(maxFee, baseFee + tip);

    // Compute fee amounts
    uint64_t baseFeeTotal  = gasUsed * baseFee;
    uint64_t priorityTotal = gasUsed * tip;
    uint64_t totalFee      = gasUsed * effPrice;

    // Deduct fees from sender
    uint64_t senderBal = chain.getBalance(tx.fromAddress);
    if (senderBal < totalFee) {
        std::cerr << "[FEE] Insufficient balance: needed "
                  << totalFee << ", available "
                  << senderBal << std::endl;
        return false;
    }
    chain.setBalance(tx.fromAddress, senderBal - totalFee);

    // Pay miner the priority fee
    uint64_t minerBal = chain.getBalance(minerAddress);
    chain.setBalance(minerAddress, minerBal + priorityTotal);

    // Burn base fee or send it to protocol treasury
    chain.burnBaseFees(baseFeeTotal);

    // Update gas used for consensus
    chain.currentBlock.gasUsed += gasUsed;

    std::cout << "[FEE] Charged fees. GasUsed=" << gasUsed
              << ", BaseFee=" << baseFeeTotal
              << ", Priority=" << priorityTotal
              << ", Total=" << totalFee << std::endl;

    return (result.status_code == EVMC_SUCCESS);
}

// -------------------------------
// TRANSACTION DISPATCH
// -------------------------------

bool processTransaction(const Transaction& tx, Blockchain& chain, const std::string& minerAddress) {
    switch (tx.type) {
        case TxType::Standard:
            return processStandardTx(tx, chain);

        case TxType::ContractDeploy:
            return processContractDeploy(tx, chain, minerAddress);

        case TxType::ContractCall:
            return processContractCall(tx, chain, minerAddress);

        default:
            std::cerr << "[TX] Unknown transaction type" << std::endl;
            return false;
    }
}

// -------------------------------
// Debug Output
// -------------------------------

void printTransaction(const Transaction& tx) {
    std::cout << "Tx Hash: " << tx.txHash << std::endl;
    std::cout << "Type: ";
    switch (tx.type) {
        case TxType::Standard:
            std::cout << "Standard UTXO"; break;
        case TxType::ContractDeploy:
            std::cout << "Contract Deploy"; break;
        case TxType::ContractCall:
            std::cout << "Contract Call"; break;
    }

    if (tx.type != TxType::Standard) {
        std::cout << " | From: " << tx.fromAddress
                  << " | To: " << tx.toAddress
                  << " | GasLimit: " << tx.gasLimit
                  << " | MaxFeePerGas: " << tx.maxFeePerGas
                  << " | MaxPriorityFeePerGas: " << tx.maxPriorityFeePerGas
                  << " | Data size: " << tx.data.size();
    }
    std::cout << std::endl;
}
