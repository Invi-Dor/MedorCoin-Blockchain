#pragma once

#ifndef PEER_MANAGER_H
#define PEER_MANAGER_H

#include <string>
#include <vector>
#include <memory>

// Forward declarations of libp2p interfaces (no real includes here)
namespace libp2p {
    namespace host {
        class Host;
    }
    namespace protocol {
        class Kademlia;   // protocol types if needed
    }
    namespace multi {
        class Multiaddress;
    }
}

// Manages peers in the network
class PeerManager {
public:
    explicit PeerManager(const std::string &listenAddr);
    ~PeerManager();

    bool start();
    bool connectTo(const std::string &peerAddr);
    void broadcast(const std::string &topic, const std::string &msg);
    void handleMessage(const std::string &topic,
                       const std::string &from,
                       const std::string &msg);

private:
    std::string listenAddress;

    // Use pointer so we don’t need full definition here
    std::shared_ptr<libp2p::host::Host> hostPtr;
};

#endif // PEER_MANAGER_H
