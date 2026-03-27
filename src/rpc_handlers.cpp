#include "rpc_handlers.h"
#include "blockchain.h"
#include "receipt.h"
#include "utxo.h" // Ensure this includes the UTXOSet class
#include <nlohmann/json.hpp>

// Assuming global objects exist
extern Blockchain globalChain;
extern UTXOSet globalUtxoSet;

// =========================
// Bridge Methods
// =========================

/**
 * Method: bridge_lockUTXO
 * Parameters: [txHash, outputIndex, evmAddress]
 * Locks a UTXO on the C++ side to trigger a mint on the EVM side.
 */
std::string rpc_bridge_lockUTXO(const nlohmann::json &params, int id) {
    nlohmann::json res;
    res["jsonrpc"] = "2.0";
    res["id"]      = id;

    if (!params.is_array() || params.size() < 3) {
        jsonError(res, -32602, "Invalid params: [txHash, index, evmAddress]");
        return res.dump();
    }

    std::string txHash  = params[0].get<std::string>();
    int index           = params[1].get<int>();
    std::string evmAddr = params[2].get<std::string>();

    if (globalUtxoSet.lockUTXO(txHash, index, evmAddr)) {
        res["result"] = {{"status", "locked"}, {"txHash", txHash}, {"index", index}};
    } else {
        jsonError(res, -32000, "UTXO locking failed - may be spent or already locked");
    }

    return res.dump();
}

/**
 * Method: bridge_unlockUTXO
 * Parameters: [medorAddress, amount]
 * Releases locked UTXOs back to the user after they burn wMEDOR.
 */
std::string rpc_bridge_unlockUTXO(const nlohmann::json &params, int id) {
    nlohmann::json res;
    res["jsonrpc"] = "2.0";
    res["id"]      = id;

    if (!params.is_array() || params.size() < 2) {
        jsonError(res, -32602, "Invalid params: [medorAddress, amount]");
        return res.dump();
    }

    std::string medorAddr = params[0].get<std::string>();
    uint64_t amount      = std::stoull(params[1].get<std::string>());

    auto lockedUtxos = globalUtxoSet.getUTXOsForAddress(medorAddr);
    uint64_t unlockedValue = 0;

    for (const auto& utxo : lockedUtxos) {
        if (utxo.isLocked && unlockedValue < amount) {
            if (globalUtxoSet.unlockUTXO(utxo.txHash, utxo.outputIndex)) {
                unlockedValue += utxo.value;
            }
        }
    }

    res["result"] = {{"unlockedValue", unlockedValue}, {"targetAmount", amount}};
    return res.dump();
}

/**
 * Method: bridge_getSwapLogs
 * Used by the .CJS relayer to poll for new wrap requests.
 */
std::string rpc_bridge_getSwapLogs(const nlohmann::json &params, int id) {
    nlohmann::json res;
    res["jsonrpc"] = "2.0";
    res["id"]      = id;

    nlohmann::json logs = nlohmann::json::array();
    
    // Logic: Iterate UTXO set and find items where isLocked == true 
    // In production, you would maintain a separate 'pendingSwaps' queue for performance.
    // For now, we query the state.
    
    // Note: This requires a helper in UTXOSet to retrieve all locked items
    // auto allLocked = globalUtxoSet.getAllLockedUTXOs();
    // for(auto& l : allLocked) logs.push_back({...});

    res["result"] = logs;
    return res.dump();
}
