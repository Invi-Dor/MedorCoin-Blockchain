#include "net/net_manager.h"
#include <libp2p/host/host.hpp>
#include <libp2p/peer/peer_id.hpp>
#include <libp2p/multi/multiaddress.hpp>
#include <libp2p/protocol/ping/ping.hpp>
#include <iostream>

// Constructor: prepare host multiaddress
NetworkManager::NetworkManager(const std::string &listenAddr) {
    libp2p::multi::Multiaddress ma(listenAddr);
    host = libp2p::host::Host::create({ma});
}

// Cleanup
NetworkManager::~NetworkManager() = default;

// Start libp2p swarm and run event loop
bool NetworkManager::start() {
    host->start();
    initProtocols();
    std::cout << "[libp2p] Node started\n";
    return true;
}

// Connect to bootstrap peers
bool NetworkManager::connectBootstrap(const std::vector<std::string> &bootstrap) {
    for (auto &addr : bootstrap) {
        libp2p::multi::Multiaddress ma(addr);
        host->getNetwork().connect(ma,
            [](auto ec) {
                if (ec) std::cerr << "[libp2p] Connection failed\n";
            });
    }
    return true;
}

// Set up protocols and stream handlers
void NetworkManager::initProtocols() {
    host->setStreamHandler(
        "/medorcoin/1.0.0",
        [&](auto stream) {
            handleStream(stream);
        });
}

// Handle incoming stream (JSON message)
void NetworkManager::handleStream(std::shared_ptr<libp2p::network::Stream> stream) {
    // read bytes from stream, parse JSON, call callback
    auto buf = stream->read();
    nlohmann::json msg = nlohmann::json::parse(buf);
    if (messageHandler) messageHandler(msg);
}

// Broadcast block message to all peers
void NetworkManager::broadcastBlock(const nlohmann::json &blockMsg) {
    std::string data = blockMsg.dump();
    for (auto &conn : host->getNetwork().getPeers()) {
        auto s = host->newStream(conn, {"/medorcoin/1.0.0"});
        s->write(data);
    }
}

// Broadcast tx message
void NetworkManager::broadcastTx(const nlohmann::json &txMsg) {
    std::string data = txMsg.dump();
    for (auto &conn : host->getNetwork().getPeers()) {
        auto s = host->newStream(conn, {"/medorcoin/1.0.0"});
        s->write(data);
    }
}

// Register callback for incoming JSON messages
void NetworkManager::onMessage(const std::function<void(const nlohmann::json &)> &handler) {
    messageHandler = handler;
}
