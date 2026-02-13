#pragma once
#include <string>
#include "receipt.h"

/**
 * Handle eth_getTransactionReceipt JSON‑RPC method.
 *
 * @param params   The RPC params (expected: [txHashString])
 * @param id       The RPC request id
 * @return JSON string response
 */
std::string rpc_getTransactionReceipt(const nlohmann::json &params, int id);
