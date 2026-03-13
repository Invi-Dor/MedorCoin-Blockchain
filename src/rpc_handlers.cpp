#include "rpc_handlers.h
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
// Helpers: common responses
// =========================

static void jsonError(nlohmann::json &res, int code, const std::string &message) {
    res["error"] = {
        {"code", code},
        {"message", message}
    };
}

// Normalize an address string by removing optional 0x prefix.
// Returns empty string if input is empty after normalization.
static std::string normalizeAddress(const std::string &addr) {
    std::string a = addr;
    if (a.rfind("0x", 0) == 0) {
        a = a.substr(2);
    }
    return a;
}

// =========================
// web3 methods
// =========================

std::string rpc_web3_clientVersion(const nlohmann::json &params, int id) {
    nlohmann::json res;
    res["jsonrpc"] = "2.0";
    res["id"]      = id;
    res["result"]  = "MedorCoin/v1.0";
    return res.dump();
}

// =========================
// net methods
// =========================

std::string rpc_net_version(const nlohmann::json &params, int id) {
    nlohmann::json res;
    res["jsonrpc"] = "2.0";
    res["id"]      = id;
    res["result"]  = std::to_string(MEDOR_CHAIN_ID);
    return res.dump();
}

// =========================
// eth methods — core
// =========================

std::string rpc_eth_blockNumber(const nlohmann::json &params, int id) {
    extern Blockchain globalChain;
    nlohmann::json res;
    res["jsonrpc"] = "2.0";
    res["id"]      = id;

    // Safe height calculation
    uint64_t height = (globalChain.chain.empty() ? 0 : globalChain.chain.size() - 1);
    res["result"] = hexUInt(height);
    return res.dump();
}

std::string rpc_eth_getBalance(const nlohmann::json &params, int id) {
    nlohmann::json res;
    res["jsonrpc"] = "2.0";
    res["id"]      = id;

    // Validate params
    if (!params.is_array() || params.size() < 1 || !params[0].is_string()) {
        jsonError(res, -32602, "Invalid params");
        return res.dump();
    }

    extern Blockchain globalChain;
    std::string addr = params[0].get<std::string>();
    addr = normalizeAddress(addr);

    if (addr.empty()) {
        jsonError(res, -32602, "Invalid address");
        return res.dump();
    }

    uint64_t bal = globalChain.getBalance(addr);
    res["result"] = hexUInt(bal);
    return res.dump();
}

std::string rpc_eth_getTransactionCount(const nlohmann::json &params, int id) {
    nlohmann::json res;
    res["jsonrpc"] = "2.0";
    res["id"]      = id;

    if (!params.is_array() || params.size() < 1 || !params[0].is_string()) {
        jsonError(res, -32602, "Invalid params");
        return res.dump();
    }

    extern Blockchain globalChain;
    std::string addr = params[0].get<std::string>();
    addr = normalizeAddress(addr);

    if (addr.empty()) {
        jsonError(res, -32602, "Invalid address");
        return res.dump();
    }

    uint64_t nonce = globalChain.getNonce(addr);
    res["result"] = hexUInt(nonce);
    return res.dump();
}

// =========================
// eth methods — transactions
// =========================

std::string rpc_eth_sendRawTransaction(const nlohmann::json &params, int id) {
    nlohmann::json res;
    res["jsonrpc"] = "2.0";
    res["id"]      = id;

    if (!params.is_array() || params.size() < 1 || !params[0].is_string()) {
        jsonError(res, -32602, "Invalid params");
        return res.dump();
    }

    std::string rawHex = params[0].get<std::string>();
    // Normalize: remove optional 0x
    if (rawHex.rfind("0x", 0) == 0) rawHex = rawHex.substr(2);

    Transaction tx = decodeRawTransaction(rawHex);

    if (!tx.isValid()) {
        jsonError(res, -32000, "Invalid transaction");
        return res.dump();
    }

    extern Blockchain globalChain;
    bool ok = globalChain.mempool.addTransaction(tx);

    if (!ok) {
        jsonError(res, -32000, "Failed to add transaction to mempool");
    } else {
        // TX hash may or may not include 0x in its field; normalize for RPC
        std::string txHash = tx.txHash;
        if (txHash.rfind("0x", 0) != 0) res["result"] = "0x" + txHash;
        else res["result"] = txHash;
    }

    return res.dump();
}

// =========================
// eth methods — receipt
// =========================

std::string rpc_eth_getTransactionReceipt(const nlohmann::json &params, int id) {
    nlohmann::json res;
    res["jsonrpc"] = "2.0";
    res["id"]      = id;

    if (!params.is_array() || params.size() < 1 || !params[0].is_string()) {
        jsonError(res, -32602, "Invalid params");
        return res.dump();
    }

    std::string txHash = params[0].get<std::string>();
    if (txHash.rfind("0x", 0) == 0) txHash = txHash.substr(2);

    Receipt receipt = getReceipt(txHash);

    if (!receipt.found) {
        jsonError(res, -32000, "Receipt not found");
        return res.dump();
    }

    res["result"] = receipt.toJson();
    return res.dump();
}
