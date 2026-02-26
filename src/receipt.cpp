#pragma once

#include "receipt.h"
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

// JSON library alias
using json = nlohmann::json;

// Convert byte array to hex string
static std::string toHex(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    for (auto byte : data) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    }
    return oss.str();
}

static std::string toHex(const std::array<uint8_t,20>& arr) {
    std::ostringstream oss;
    for (auto byte : arr) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    }
    return oss.str();
}

static std::string toHex(const std::array<uint8_t,32>& arr) {
    std::ostringstream oss;
    for (auto byte : arr) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    }
    return oss.str();
}

// Serialize ReceiptLog to JSON
json serializeLog(const ReceiptLog& log) {
    json j;
    j["address"] = toHex(log.address);

    std::vector<std::string> topics;
    for (const auto& t : log.topics) {
        topics.push_back(toHex(t));
    }
    j["topics"] = topics;
    j["data"] = toHex(log.data);

    return j;
}

// Serialize TransactionReceipt to JSON
json serializeReceipt(const TransactionReceipt& r) {
    json j;
    j["transactionHash"] = r.transactionHash;
    j["blockHash"] = r.blockHash;
    j["blockNumber"] = r.blockNumber;
    j["transactionIndex"] = r.transactionIndex;
    j["from"] = toHex(r.from);
    j["to"] = toHex(r.to);
    j["cumulativeGasUsed"] = r.cumulativeGasUsed;
    j["gasUsed"] = r.gasUsed;
    j["status"] = r.status;

    std::vector<json> logsArr;
    for (const auto& l : r.logs) {
        logsArr.push_back(serializeLog(l));
    }
    j["logs"] = logsArr;

    return j;
}
