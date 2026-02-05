#pragma once

#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <string>
#include <vector>
#include <memory>

// Forward declaration to avoid missing libp2p includes
class PeerManager;
namespace libp2p {
    namespace host {
        class Host;
    }
}

// Network manager class
class NetworkManager {
public:
    explicit NetworkManager(const std::string &listenAddr);
    ~NetworkManager();

    bool start();
    bool connectBootstrap(const std::vector<std::string> &bootstrap);
    void broadcastBlock(const std::string &blockHex);
    void broadcastTx(const std::string &txHex);

private:
    PeerManager* peerMgr;  // pointer to PeerManager to avoid including its header here
    std::shared_ptr<libp2p::host::Host> hostPtr;
};

#endif // NET_MANAGER_H
