#include "net/serialization.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>

using json = nlohmann::json;

// ------------------------ Transaction Serialization ------------------------

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
    // tx.data is binary; preserve as binary blob (base64 in JSON via json::binary)
    j["data"] = json::binary(tx.data);
    return j;
}

Transaction deserializeTx(const json &j) {
    Transaction tx;
    try {
        // Required fields
        if (!j.contains("txHash") || !j.contains("fromAddress") || !j.contains("toAddress") ||
            !j.contains("value") || !j.contains("gasLimit") || !j.contains("maxFeePerGas") ||
            !j.contains("maxPriorityFeePerGas") || !j.contains("nonce") || !j.contains("data")) {
            throw std::runtime_error("Missing one or more required Transaction fields");
        }

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

// ------------------------ Block Serialization ------------------------

json serializeBlock(const Block &block) {
    json j;
    j["previousHash"] = block.previousHash;
    j["timestamp"] = block.timestamp;
    j["hash"] = block.hash;
    j["signature"] = block.signature;

    j["transactions"] = json::array();
    for (const auto &tx : block.transactions) {
        j["transactions"].push_back(serializeTx(tx));
    }

    return j;
}

Block deserializeBlock(const json &j) {
    Block block;
    try {
        if (!j.contains("previousHash") || !j.contains("timestamp") ||
            !j.contains("hash") || !j.contains("signature") || !j.contains("transactions")) {
            throw std::runtime_error("Missing one or more required Block fields");
        }

        block.previousHash = j.at("previousHash").get<std::string>();
        block.timestamp = j.at("timestamp").get<uint64_t>();
        block.hash = j.at("hash").get<std::string>();
        block.signature = j.at("signature").get<std::string>();

        block.transactions.clear();
        const auto &txArray = j.at("transactions");
        if (!txArray.is_array()) {
            throw std::runtime_error("Block field 'transactions' is not an array");
        }

        for (const auto &txJson : txArray) {
            block.transactions.push_back(deserializeTx(txJson));
        }
    } catch (const json::exception &e) {
        throw std::runtime_error(std::string("Failed to deserialize Block: ") + e.what());
    }

    return block;
}
