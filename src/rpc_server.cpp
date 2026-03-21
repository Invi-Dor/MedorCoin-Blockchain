#include "rpc_server.h"
#include "net/serialization.h"
#include "blockchain.h"
#include "utxo.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

// =============================================================================
// RpcServer
// =============================================================================

RpcServer::RpcServer(int port)
    : port_(port)
{}

RpcServer::~RpcServer() {}

void RpcServer::on(const std::string& method, RpcHandler handler)
{
    handlers_[method] = std::move(handler);
}

RpcResponse RpcServer::makeError(const json& id, int code,
                                  const std::string& msg)
{
    RpcResponse r;
    r.id    = id;
    r.error = { {"code", code}, {"message", msg} };
    return r;
}

RpcResponse RpcServer::dispatch(const RpcRequest& req) const
{
    auto it = handlers_.find(req.method);
    if (it == handlers_.end())
        return makeError(req.id, -32601,
                         "Method not found: " + req.method);
    try {
        return it->second(req);
    } catch (const std::exception& e) {
        return makeError(req.id, -32603,
                         std::string("Internal error: ") + e.what());
    } catch (...) {
        return makeError(req.id, -32603, "Unknown internal error");
    }
}

void RpcServer::listen()
{
    // Production deployment note:
    // Replace this stub with your actual JSON-RPC transport layer.
    // Options: Boost.Beast (HTTP/WebSocket), cpp-httplib, or a raw TCP loop.
    // The dispatch() method above is transport-agnostic -- wire it to
    // whatever server reads a JSON body and writes a JSON response.
    std::cout << "[RpcServer] Listening on port " << port_ << "\n";
    std::cout << "[RpcServer] Wire RpcServer::dispatch() to your "
                 "JSON-RPC transport layer.\n";
}

// =============================================================================
// startRpcServer
// Registers all JSON-RPC method handlers and starts the server.
// Blockchain and UTXOSet are passed in by reference -- no globals.
// =============================================================================

void startRpcServer(Blockchain &blockchain,
                    UTXOSet    &utxoSet,
                    int         port)
{
    RpcServer server(port);

    // -------------------------------------------------------------------------
    // eth_getBalance
    // params: ["<address>", "latest"]
    // -------------------------------------------------------------------------
    server.on("eth_getBalance", [&](const RpcRequest& req) -> RpcResponse {
        RpcResponse r;
        r.id = req.id;
        if (!req.params.is_array() || req.params.empty()) {
            r.error = { {"code", -32602}, {"message", "missing params"} };
            return r;
        }
        std::string addr = req.params[0].get<std::string>();
        uint64_t balance = utxoSet.getBalance(addr).value_or(0);
        r.result = balance;
        return r;
    });

    // -------------------------------------------------------------------------
    // eth_blockNumber
    // -------------------------------------------------------------------------
    server.on("eth_blockNumber", [&](const RpcRequest& req) -> RpcResponse {
        RpcResponse r;
        r.id     = req.id;
        r.result = blockchain.height();
        return r;
    });

    // -------------------------------------------------------------------------
    // eth_getBlockByNumber
    // params: ["latest" | <number>, <full_tx: bool>]
    // -------------------------------------------------------------------------
    server.on("eth_getBlockByNumber", [&](const RpcRequest& req) -> RpcResponse {
        RpcResponse r;
        r.id = req.id;
        auto latestOpt = blockchain.getLatestBlock();
        if (!latestOpt) {
            r.result = nullptr;
            return r;
        }
        r.result = serializeBlock(*latestOpt);
        return r;
    });

    // -------------------------------------------------------------------------
    // eth_sendRawTransaction
    // params: ["<json-serialized transaction>"]
    // -------------------------------------------------------------------------
    server.on("eth_sendRawTransaction", [&](const RpcRequest& req) -> RpcResponse {
        RpcResponse r;
        r.id = req.id;
        if (!req.params.is_array() || req.params.empty()) {
            r.error = { {"code", -32602}, {"message", "missing params"} };
            return r;
        }
        try {
            std::string raw = req.params[0].get<std::string>();
            Transaction tx  = deserializeTx(json::parse(raw));
            std::string minerAddr = tx.outputs.empty()
                ? "" : tx.outputs.front().address;
            bool ok = blockchain.addBlock(minerAddr, { tx });
            if (!ok) {
                r.error = { {"code", -32000},
                            {"message", "transaction rejected"} };
            } else {
                r.result = tx.txHash;
            }
        } catch (const std::exception& e) {
            r.error = { {"code", -32602},
                        {"message", std::string("deserialization failed: ")
                                    + e.what()} };
        }
        return r;
    });

    // -------------------------------------------------------------------------
    // eth_gasPrice
    // -------------------------------------------------------------------------
    server.on("eth_gasPrice", [&](const RpcRequest& req) -> RpcResponse {
        RpcResponse r;
        r.id     = req.id;
        r.result = blockchain.baseFee();
        return r;
    });

    // -------------------------------------------------------------------------
    // medor_totalSupply
    // -------------------------------------------------------------------------
    server.on("medor_totalSupply", [&](const RpcRequest& req) -> RpcResponse {
        RpcResponse r;
        r.id     = req.id;
        r.result = blockchain.totalSupply();
        return r;
    });

    server.listen();
}
