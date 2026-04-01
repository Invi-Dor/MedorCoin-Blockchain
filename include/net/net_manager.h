#pragma once

#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

class NetworkManager {
public:
    struct Metrics {
        size_t   connectedPeers;
        size_t   trustedPeers;
        size_t   bannedPeers;
        uint64_t msgReceived;
        uint64_t msgSent;
        uint64_t bytesReceived;
        uint64_t bytesSent;
        uint64_t totalConnections;
        uint64_t totalDisconnections;
        uint64_t totalBanned;
        uint64_t rateLimited;
        uint64_t handshakeFailed;
        uint64_t broadcastSent;
        uint64_t broadcastFailed;
    };

    explicit NetworkManager(const std::string &listenAddr);
    ~NetworkManager();

    NetworkManager(const NetworkManager&)            = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    void setLogger(std::function<void(int,
                   const std::string&)> fn);

    bool start();
    void stop();

    bool connectBootstrap(
        const std::vector<std::string> &bootstrap);

    void addPeer(const std::string              &id,
                  const std::string              &host,
                  int                             port,
                  bool                            trusted,
                  int                             priority,
                  const std::string              &region,
                  const std::vector<std::string> &tags);

    void removePeer(const std::string &id);
    void banPeer   (const std::string &id,
                     const std::string &reason);

    void broadcastBlock(const nlohmann::json &blockMsg);
    void broadcastTx   (const nlohmann::json &txMsg);

    void onMessage   (std::function<void(const nlohmann::json&)> handler);
    void onConnect   (std::function<void(const std::string&)>    handler);
    void onDisconnect(std::function<void(const std::string&)>    handler);

    void receiveRaw(const std::string &peerId,
                     const std::string &raw);

    size_t  peerCount()        const noexcept;
    size_t  trustedPeerCount() const noexcept;
    Metrics getMetrics()       const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
