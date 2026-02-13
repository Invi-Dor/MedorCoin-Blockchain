#include "rpc_server.h"
#include "rpc_handlers.h"
#include <your_rpc_library.hpp> // whatever JSON-RPC lib you’re using

void startRpcServer(int port) {
    RpcServer server(port);  // example: your JSON‑RPC server class

    // Register eth_getTransactionReceipt
    server.on("eth_getTransactionReceipt", [&](const RpcRequest &request) {
        return rpc_getTransactionReceipt(request.params, request.id);
    });

    // Register other methods…
    server.on("eth_sendRawTransaction", [&](const RpcRequest &request) {
        return rpc_sendRawTransaction(request.params, request.id);
    });

    // …add more handlers as needed

    server.listen();
}
