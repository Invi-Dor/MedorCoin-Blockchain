#pragma once

#include "blockchain.h"
#include "utxo.h"
#include "net/serialization.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

// =============================================================================
// RPC SERVER
//
// Minimal JSON-RPC 2.0 server for MedorCoin node.
// Handlers are registered by method name and dispatched on each request.
// Thread-safe dispatch via shared_mutex.
// =============================================================================

struct RpcRequest {
    std::string  method;
    json         params;
    json         id;
};

struct RpcResponse {
    json result;
    json error;
    json id;

    std::string toJson() const
    {
        json out;
        out["jsonrpc"] = "2.0";
        out["id"]      = id;
        if (!error.is_null())
            out["error"]  = error;
        else
            out["result"] = result;
        return out.dump();
    }
};

using RpcHandler = std::function<RpcResponse(const RpcRequest&)>;

class RpcServer {
public:
    explicit RpcServer(int port);
    ~RpcServer();

    RpcServer(const RpcServer&)            = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    void on(const std::string& method, RpcHandler handler);
    void listen();

private:
    int port_;
    std::unordered_map<std::string, RpcHandler> handlers_;

    RpcResponse dispatch(const RpcRequest& req) const;
    static RpcResponse makeError(const json& id, int code,
                                  const std::string& msg);
};

void startRpcServer(Blockchain &blockchain,
                    UTXOSet    &utxoSet,
                    int         port = 8545);
