#pragma once
#include "net/peer_manager.h"
#include <vector>
#include <string>

class NetworkManager {
public:
    NetworkManager(const std::string &listenAddr);

    bool start();
    bool connectBootstrap(const std::vector<std::string> &bootstrap);
    void broadcastBlock(const std::string &blockHex);
    void broadcastTx(const std::string &txHex);

private:
    PeerManager peerMgr;
};
