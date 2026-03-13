#pragma once

#include <string>
#include <nlohmann/json.hpp>

/**
 * JSON‑RPC handler functions — each returns a serialized JSON string
 * according to the JSON‑RPC 2.0 specification.
 *
 * @param params  A JSON array of parameters from the RPC request
 * @param id      The JSON‑RPC request id
 * @return        A std::string of the serialized JSON object
 */

// web3 methods
std::string rpc_web3_clientVersion(const nlohmann::json &params, int id);

// net methods
std::string rpc_net_version(const nlohmann::json &params, int id);

// eth core methods
std::string rpc_eth_blockNumber(const nlohmann::json &params, int id);
std::string rpc_eth_getBalance(const nlohmann::json &params, int id);
std::string rpc_eth_getTransactionCount(const nlohmann::json &params, int id);

// eth transactions
std::string rpc_eth_sendRawTransaction(const nlohmann::json &params, int id);

// eth receipt
std::string rpc_eth_getTransactionReceipt(const nlohmann::json &params, int id);
