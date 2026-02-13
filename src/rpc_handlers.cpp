#include "rpc_handlers.h"
#include "blockchain.h"
#include "receipt.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>

// Helpers: format hex values
static std::string toHex(const std::vector<uint8_t> &bytes) {
    std::ostringstream ss;
    ss << "0x";
    for (auto b : bytes) ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return ss.str();
}

static std::string hexUInt(uint64_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << v;
    return ss.str();
}

std::string rpc_getTransactionReceipt(const nlohmann::json &params, int id) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;

    if (!params.is_array() || params.size() != 1) {
        response["error"] = {{"code", -32602}, {"message", "Invalid params"}};
        return response.dump();
    }

    std::string txHash = params[0].get<std::string>();

    auto optReceipt = ReceiptStore::getReceiptByHash(txHash);
    if (!optReceipt) {
        response["result"] = nullptr;
        return response.dump();
    }

    const TransactionReceipt &r = *optReceipt;

    nlohmann::json receiptJson;
    receiptJson["transactionHash"]  = txHash;
    receiptJson["transactionIndex"] = hexUInt(r.transactionIndex);
    receiptJson["blockHash"]        = "0x" + r.blockHash;
    receiptJson["blockNumber"]      = hexUInt(r.blockNumber);
    receiptJson["from"]             = toHex(std::vector<uint8_t>(r.from.begin(), r.from.end()));
    receiptJson["to"]               = toHex(std::vector<uint8_t>(r.to.begin(), r.to.end()));

    // Contract creation address (null if not created)
    if (r.contractAddress.has_value()) {
        receiptJson["contractAddress"] = toHex(std::vector<uint8_t>(r.contractAddress->begin(),
                                                                    r.contractAddress->end()));
    } else {
        receiptJson["contractAddress"] = nullptr;
    }

    receiptJson["cumulativeGasUsed"]   = hexUInt(r.cumulativeGasUsed);
    receiptJson["gasUsed"]             = hexUInt(r.gasUsed);
    receiptJson["effectiveGasPrice"]   = hexUInt(r.effectiveGasPrice);

    receiptJson["logsBloom"] = toHex(std::vector<uint8_t>(r.logsBloom.begin(),
                                                           r.logsBloom.end()));

    receiptJson["logs"] = nlohmann::json::array();
    for (auto &log : r.logs) {
        nlohmann::json logJson;
        logJson["address"] = toHex(std::vector<uint8_t>(log.address.begin(),
                                                        log.address.end()));

        logJson["topics"] = nlohmann::json::array();
        for (auto &topic : log.topics) {
            logJson["topics"].push_back(toHex(std::vector<uint8_t>(topic.begin(),
                                                                   topic.end())));
        }

        logJson["data"] = toHex(log.data);
        receiptJson["logs"].push_back(logJson);
    }

    receiptJson["status"] = r.status ? "0x1" : "0x0";
    receiptJson["type"]   = hexUInt(r.txType);

    response["result"] = receiptJson;
    return response.dump();
}
