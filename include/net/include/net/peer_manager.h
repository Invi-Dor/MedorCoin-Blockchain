#pragma once

#include <libp2p/host/host.hpp>
#include <libp2p/security/noise/noise.hpp>
#include <libp2p/transport/tcp/tcp.hpp>
#include <libp2p/multi/multiaddress.hpp>
#include <libp2p/protocol/kademlia/kademlia.hpp>
#include <string>
#include <vector>

class PeerManager {
public:
    PeerManager(const std::string &listenAddr);

    bool start();
    bool connectTo(const std::string &peerAddr);
    void broadcast(const std::string &topic, const std::string &msg);
    void handleMessage(const std::string &topic, const std::string &from, const std::string &msg);

private:
    std::string listenAddress;
    std::shared_ptr<libp2p::host::Host> host;
};
