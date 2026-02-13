#pragma once

#include <string>
#include <nlohmann/json.hpp>

// --- web3 namespace ---
std::string rpc_web3_clientVersion(const nlohmann::json &params, int id);

// --- net namespace ---
std::string rpc_net_version(const nlohmann::json &params, int id);

// --- eth namespace --- core methods
std::string rpc_eth_blockNumber(const nlohmann::json &params, int id);
std::string rpc_eth_getBalance(const nlohmann::json &params, int id);
std::string rpc_eth_getTransactionCount(const nlohmann::json &params, int id);
std::string rpc_eth_sendRawTransaction(const nlohmann::json &params, int id);
std::string rpc_eth_getTransactionByHash(const nlohmann::json &params, int id);
std::string rpc_eth_getTransactionReceipt(const nlohmann::json &params, int id);

// --- optional but common eth methods ---
std::string rpc_eth_getCode(const nlohmann::json &params, int id);
std::string rpc_eth_call(const nlohmann::json &params, int id);
std::string rpc_eth_estimateGas(const nlohmann::json &params, int id);
