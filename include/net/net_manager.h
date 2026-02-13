#pragma once
#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

namespace libp2p {
    namespace host { class Host; }
    namespace network { class Stream; class PeerId; }
}

// Manages libp2p peer connections and message handlers
class NetworkManager {
public:
    explicit NetworkManager(const std::string &listenAddr);
    ~NetworkManager();

    // Start listening on libp2p swarm
    bool start();

    // Connect to bootstrap multiaddresses
    bool connectBootstrap(const std::vector<std::string> &bootstrap);

    // Send a JSON block message to peers
    void broadcastBlock(const nlohmann::json &blockMsg);

    // Send a JSON tx message to peers
    void broadcastTx(const nlohmann::json &txMsg);

    // Register a callback to handle incoming messages
    void onMessage(const std::function<void(const nlohmann::json &)> &handler);

private:
    std::shared_ptr<libp2p::host::Host> host;
    std::function<void(const nlohmann::json &)> messageHandler;

    void initProtocols();
    void handleStream(std::shared_ptr<libp2p::network::Stream> stream);
};

#endif // NET_MANAGER_H
