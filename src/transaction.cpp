#include "transaction.h"
#include "crypto.h"    // doubleSHA256
#include <vector>
#include <cstdint>
#include <string>
#include <evmc/evmc.hpp> // EVM address/types if needed
#include <iostream>

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
    // Version
    writeUInt32LE(buf, tx.version);

    // Input count
    buf.push_back(static_cast<unsigned char>(tx.inputs.size()));

    // Inputs
    for (auto& in : tx.inputs) {
        for (size_t i = 0; i < 32 && i < in.prevTxHash.size(); ++i)
            buf.push_back(static_cast<unsigned char>(in.prevTxHash[i]));

        writeUInt32LE(buf, in.outputIndex);

        // Simplified scriptSig length
        buf.push_back(0);

        // Sequence
        writeUInt32LE(buf, 0xffffffff);
    }

    // Output count
    buf.push_back(static_cast<unsigned char>(tx.outputs.size()));

    // Outputs
    for (auto& out : tx.outputs) {
        writeUInt64LE(buf, out.value);

        // Simplified scriptPubKey length
        buf.push_back(0);
    }

    // Locktime
    writeUInt32LE(buf, tx.lockTime);
}

// -------------------------------
// Serialize EVM Transaction
// -------------------------------

static void serializeEVM(const Transaction& tx, std::vector<unsigned char>& buf) {
    // Version
    writeUInt32LE(buf, tx.version);

    // Type
    buf.push_back(static_cast<unsigned char>(tx.type));

    // From address
    buf.insert(buf.end(), tx.fromAddress.begin(), tx.fromAddress.end());

    // To address (empty for deploy)
    buf.insert(buf.end(), tx.toAddress.begin(), tx.toAddress.end());

    // Gas limit
    writeUInt64LE(buf, tx.gasLimit);

    // Data length
    uint32_t dataLen = static_cast<uint32_t>(tx.data.size());
    writeUInt32LE(buf, dataLen);

    // Data
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
// Debug: Print Transaction Info
// -------------------------------

void printTransaction(const Transaction& tx) {
    std::cout << "Tx Hash: " << tx.txHash << std::endl;
    std::cout << "Type: ";
    switch (tx.type) {
        case TxType::Standard: std::cout << "Standard UTXO"; break;
        case TxType::ContractDeploy: std::cout << "Contract Deploy"; break;
        case TxType::ContractCall: std::cout << "Contract Call"; break;
    }
    std::cout << std::endl;

    if (tx.type == TxType::Standard) {
        std::cout << "Inputs: " << tx.inputs.size() << ", Outputs: " << tx.outputs.size() << std::endl;
    } else {
        std::cout << "From: " << tx.fromAddress << ", To: " << tx.toAddress
                  << ", Gas: " << tx.gasLimit << ", Data size: " << tx.data.size() << std::endl;
    }
}
