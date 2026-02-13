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
    resp["result"]  = "MedorCoin/v1.0";
    return resp.dump();
}

// =========================
// net methods
// =========================

std::string rpc_net_version(const nlohmann::json &params, int id) {
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    // Example: MedorCoin chain ID as decimal
    resp["result"]  = std::to_string(MEDOR_CHAIN_ID);
    return resp.dump();
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
    return resp.dump();
}

std::string rpc_eth_getBalance(const nlohmann::json &params, int id) {
    if (!params.is_array() || params.size() < 1) {
        nlohmann::json r; r["jsonrpc"] = "2.0"; r["id"] = id;
        r["error"] = {{"code",-32602},{"message","Invalid params"}};
        return r.dump();
    }
    extern Blockchain globalChain;
    std::string addr = params[0].get<std::string>();
    if (addr.rfind("0x",0) == 0) addr = addr.substr(2);
    uint64_t bal = globalChain.getBalance(addr);
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["result"]  = hexUInt(bal);
    return resp.dump();
}

std::string rpc_eth_getTransactionCount(const nlohmann::json &params, int id) {
    if (!params.is_array() || params.size() < 1) {
        nlohmann::json r; r["jsonrpc"]="2.0"; r["id"]=id;
        r["error"]={{"code",-32602},{"message","Invalid params"}};
        return r.dump();
    }
    extern Blockchain globalChain;
    std::string addr = params[0].get<std::string>();
    if (addr.rfind("0x",0) == 0) addr = addr.substr(2);
    uint64_t nonce = globalChain.getNonce(addr);
    nlohmann::json resp;
    resp["jsonrpc"]="2.0";
    resp["id"]=id;
    resp["result"]=hexUInt(nonce);
    return resp.dump();
}

// Assumes decodeRawTransaction and mempool exist
std::string rpc_eth_sendRawTransaction(const nlohmann::json &params, int id) {
    nlohmann::json resp;
    resp["jsonrpc"]="2.0";
    resp["id"]=id;

    if (!params.is_array() || params.size()<1) {
        resp["error"]={{"code",-32602},{"message","Invalid params"}};
        return resp.dump();
    }

    std::string rawHex=params[0].get<std::string>();
    Transaction tx = decodeRawTransaction(rawHex);
    std::string reason;
    bool ok = globalMempool.addTransaction(tx);
    if (!ok) {
        resp["error"]={{"code",-32000},{"message",reason}};
    } else {
        resp["result"] = "0x" + tx.txHash;
    }
    return resp.dump();
}

std::string rpc_eth_getTransactionByHash(const nlohmann::json &params, int id) {
    nlohmann::json resp;
    resp["jsonrpc"]="2.0";
    resp["id"]=id;
    if (!params.is_array() || params.size()<1) {
        resp["error"]={{"code",-32602},{"message","Invalid params"}};
        return resp.dump();
    }
    std::string hash=params[0].get<std::string>();
    Transaction tx;
    bool found = globalChain.findTransaction(hash, tx);
    if (!found) {
        resp["result"] = nullptr;
    } else {
        resp["result"] = serializeTransactionJson(tx);
    }
    return resp.dump();
}

std::string rpc_eth_getTransactionReceipt(const nlohmann::json &params, int id) {
    return rpc_getTransactionReceipt(params,id);
}

// =========================
// eth methods — optional stubs
// =========================

std::string rpc_eth_getCode(const nlohmann::json &params, int id) {
    nlohmann::json resp;
    resp["jsonrpc"]="2.0";
    resp["id"]=id;
    resp["result"]="0x";
    return resp.dump();
}

std::string rpc_eth_call(const nlohmann::json &params, int id) {
    nlohmann::json resp;
    resp["jsonrpc"]="2.0";
    resp["id"]=id;
    resp["result"]="0x";
    return resp.dump();
}

std::string rpc_eth_estimateGas(const nlohmann::json &params, int id) {
    nlohmann::json resp;
    resp["jsonrpc"]="2.0";
    resp["id"]=id;
    resp["result"]=hexUInt(21000);
    return resp.dump();
}
