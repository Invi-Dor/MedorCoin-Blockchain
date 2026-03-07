#include "rpc_handlers.h"
#include "blockchain.h"
#include "receipt.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>

// =========================
// Helpers: hex formatting
// =========================

static std::string hexUInt(uint64_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << v;
    return ss.str();
}

// =========================
// web3 methods
// =========================

std::string rpc_web3_clientVersion(const nlohmann::json &params, int id) {
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    response["result"]  = "MedorCoin/v1.0";
    response["id"]      = id;
    return response;
}

// =========================
// net methods
// =========================

std::string rpc_net_version(const nlohmann::json &params, int id) {
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["result"]  = std::to_string(MEDOR_CHAIN_ID);
    return response;
}

// =========================
// eth methods — core
// =========================

std::string rpc_eth_blockNumber(const nlohmann::json &params, int id) {
    extern Blockchain globalChain;
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["result"]  = hexUInt(globalChain.chain.size() - 1);
    return response;
}

std::string rpc_eth_getBalance(const nlohmann::json &params, int id) {
    if (!params.is_array() || params.size() < 1) {
        nlohmann::json r; r["jsonrpc"] = "2.0"; r["id"] = id;
        r["error"] = {{"code",-32602},{"message","Invalid params"}};
        return r.;
    }
    extern Blockchain globalChain;
    std::string addr = params[0].get<std::string>();
    if (addr.rfind("0x",0) == 0) addr = addr.substr(2);
    uint64_t bal = globalChain.getBalance(addr);
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["result"]  = hexUInt(bal);
    return resp.;
}

std::string rpc_eth_getTransactionCount(const nlohmann::json &params, int id) {
    if (!params.is_array() || params.size() < 1) {
        nlohmann::json r; r["jsonrpc"]="2.0"; r["id"]=id;
        r["error"]={{"code",-32602},{"message","Invalid params"}};
        return r.;
    }
    extern Blockchain globalChain;
    std::string addr = params[0].get<std::string>();
    if (addr.rfind("0x",0) == 0) addr = addr.substr(2);
    uint64_t nonce = globalChain.getNonce(addr);
    nlohmann::json resp;
    resp["jsonrpc"]="2.0";
    resp["id"]=id;
    resp["result"]=hexUInt(nonce);
    return resp.;
}

// This function adds a raw transaction to the mempool
std::string rpc_eth_sendRawTransaction(const nlohmann::json &params, int id) {
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;

    if (!params.is_array() || params.size() < 1) {
        resp["error"] = {{"code", -32602}, {"message", "Invalid params"}};
        return resp.;
    }

    std::string rawHex = params[0].get<std::string>();
    Transaction tx = decodeRawTransaction(rawHex);
    
    // Optionally, validate the transaction before adding it
    if (!tx.isValid()) {
        resp["error"] = {{"code", -32000}, {"message", "Invalid transaction"}};
        return resp.;
    }

    bool ok = globalMempool.addTransaction(tx);
    if (!ok) {
        resp["error"] = {{"code", -32000}, {"message", "Failed to add transaction to mempool"}};
    } else {
        resp["result"] = "0x" + tx.txHash; // Return transaction hash
    }
    
    return resp.;
}

// Other methods remain unchanged...
