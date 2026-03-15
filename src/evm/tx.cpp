#include "transaction.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>

using json = nlohmann::json;

// Build a Transaction from explicit fields (validation performed by caller)
Transaction makeTransaction(
    const std::string &txHash,
    const std::string &fromAddress,
    const std::string &toAddress,
    uint64_t value,
    uint64_t gasLimit,
    uint64_t maxFeePerGas,
    uint64_t maxPriorityFeePerGas,
    uint64_t nonce,
    const std::vector<uint8_t> &data
) {
    Transaction tx;
    tx.txHash = txHash;
    tx.fromAddress = fromAddress;
    tx.toAddress = toAddress;
    tx.value = value;
    tx.gasLimit = gasLimit;
    tx.maxFeePerGas = maxFeePerGas;
    tx.maxPriorityFeePerGas = maxPriorityFeePerGas;
    tx.nonce = nonce;
    tx.data = data;
    return tx;
}

// Serialize a Transaction to JSON (transport-ready)
json serializeTx(const Transaction &tx) {
    json j;
    j["txHash"] = tx.txHash;
    j["fromAddress"] = tx.fromAddress;
    j["toAddress"] = tx.toAddress;
    j["value"] = tx.value;
    j["gasLimit"] = tx.gasLimit;
    j["maxFeePerGas"] = tx.maxFeePerGas;
    j["maxPriorityFeePerGas"] = tx.maxPriorityFeePerGas;
    j["nonce"] = tx.nonce;
    j["data"] = json::binary(tx.data);
    return j;
}

// Deserialize JSON into a Transaction
Transaction deserializeTx(const json &j) {
    Transaction tx;
    if (!j.contains("txHash") || !j.contains("fromAddress") || !j.contains("toAddress") ||
        !j.contains("value") || !j.contains("gasLimit") || !j.contains("maxFeePerGas") ||
        !j.contains("maxPriorityFeePerGas") || !j.contains("nonce") || !j.contains("data")) {
        throw std::runtime_error("Missing required Transaction fields during deserialization");
    }
    try {
        tx.txHash = j.at("txHash").get<std::string>();
        tx.fromAddress = j.at("fromAddress").get<std::string>();
        tx.toAddress = j.at("toAddress").get<std::string>();
        tx.value = j.at("value").get<uint64_t>();
        tx.gasLimit = j.at("gasLimit").get<uint64_t>();
        tx.maxFeePerGas = j.at("maxFeePerGas").get<uint64_t>();
        tx.maxPriorityFeePerGas = j.at("maxPriorityFeePerGas").get<uint64_t>();
        tx.nonce = j.at("nonce").get<uint64_t>();
        tx.data = j.at("data").get<std::vector<uint8_t>>();
    } catch (const json::exception &e) {
        throw std::runtime_error(std::string("Failed to deserialize Transaction: ") + e.what());
    }
    return tx;
}
