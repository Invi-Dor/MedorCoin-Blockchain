#ifndef MEDOR_RPC_HANDLERS_H
#define MEDOR_RPC_HANDLERS_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

/**
 * MEDORCOIN PRODUCTION RPC HANDLERS
 * - Standard Web3/Eth-compatible methods
 * - Industrial Bridge/Swap extensions
 */

// --- Web3 & Network Methods ---
std::string rpc_web3_clientVersion(const nlohmann::json &params, int id);
std::string rpc_net_version(const nlohmann::json &params, int id);

// --- Blockchain State Methods ---
std::string rpc_eth_blockNumber(const nlohmann::json &params, int id);
std::string rpc_eth_getBalance(const nlohmann::json &params, int id);
std::string rpc_eth_getTransactionCount(const nlohmann::json &params, int id);

// --- Transaction Methods ---
std::string rpc_eth_sendRawTransaction(const nlohmann::json &params, int id);
std::string rpc_eth_getTransactionReceipt(const nlohmann::json &params, int id);

// --- Bridge & Swap Extensions (Production Grade) ---

/**
 * bridge_lockUTXO
 * Marks a UTXO as locked for cross-chain wrapping.
 * Params: [txHash, index, evmAddress]
 */
std::string rpc_bridge_lockUTXO(const nlohmann::json &params, int id);

/**
 * bridge_unlockUTXO
 * Releases locked UTXOs back to the user upon EVM burn confirmation.
 * Params: [medorAddress, amount]
 */
std::string rpc_bridge_unlockUTXO(const nlohmann::json &params, int id);

/**
 * bridge_getSwapLogs
 * Returns a list of all UTXOs currently in 'Locked' state awaiting relayer action.
 * Params: []
 */
std::string rpc_bridge_getSwapLogs(const nlohmann::json &params, int id);

/**
 * bridge_getProof
 * Generates a Merkle Proof for a specific transaction to allow SPV validation on-chain.
 * Params: [txHash]
 */
std::string rpc_bridge_getProof(const nlohmann::json &params, int id);

// --- Helper: JSON Error Formatter ---
void jsonError(nlohmann::json &res, int code, const std::string &message);

#endif // MEDOR_RPC_HANDLERS_H
