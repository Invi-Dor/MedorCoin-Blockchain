#pragma once

#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

class PeerManager;

namespace libp2p {
    namespace host {
        class Host;
    }
}

/**
 * NetworkManager — abstracts P2P communication.
 * Allows node to broadcast blocks/txs and register
 * a message handler callback for incoming JSON messages.
 */
class NetworkManager {
public:
    explicit NetworkManager(const std::string &listenAddr);
    ~NetworkManager();

    // Start listening/peer subsystem
    bool start();

    // Connect to bootstrap peers
    bool connectBootstrap(const std::vector<std::string> &bootstrap);

    // Broadcast a block or chain message
    void broadcastBlock(const nlohmann::json &blockMsg);

    // Broadcast transactions
    void broadcastTx(const nlohmann::json &txMsg);

    /**
     * Register a callback for incoming JSON messages
     * from the network layer.
     */
    void onMessage(const std::function<void(const nlohmann::json &)> &handler);

private:
    PeerManager *peerMgr;
    std::shared_ptr<libp2p::host::Host> hostPtr;
};

#endif // NET_MANAGER_H
