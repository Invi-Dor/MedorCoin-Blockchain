#pragma once

#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

// Forward declare PeerManager to avoid including its header here
class PeerManager;

namespace libp2p {
    namespace host {
        class Host;
    }
}

class NetworkManager {
public:
    explicit NetworkManager(const std::string &listenAddr);
    ~NetworkManager();

    // Start listening for incoming connections
    bool start();

    // Connect to a list of bootstrap peers
    bool connectBootstrap(const std::vector<std::string> &bootstrap);

    // Broadcast a signed block message (JSON) over P2P
    void broadcastBlock(const nlohmann::json &blockMsg);

    // Broadcast a transaction message (JSON)
    void broadcastTx(const nlohmann::json &txMsg);

    // Register a message handler callback for incoming JSON messages
    void onMessage(const std::function<void(const nlohmann::json &)> &handler);

private:
    PeerManager* peerMgr;  // abstract peer manager pointer
    std::shared_ptr<libp2p::host::Host> hostPtr;  // optional libp2p host
};

#endif // NET_MANAGER_H
