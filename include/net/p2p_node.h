#pragma once#pragma once

#include "net/peer_manager.h"
#include "net/message_handler.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net {

// =============================================================================
// METRICS
// =============================================================================
struct P2PNodeMetrics {
    uint64_t sessionsCreated;
    uint64_t sessionsDestroyed;
    uint64_t connectAttempts;
    uint64_t connectSuccess;
    uint64_t connectFailed;
    uint64_t acceptSuccess;
    uint64_t acceptRejected;
    uint64_t framesSent;
    uint64_t framesRecv;
    uint64_t bytesSent;
    uint64_t bytesRecv;
    uint64_t sendErrors;
    uint64_t recvErrors;
    uint64_t oversizedFrames;
    uint64_t tlsHandshakeFailed;
    uint64_t reconnectAttempts;
    uint64_t peersBanned;
    uint64_t peersEvicted;
    uint64_t broadcastsSent;
    uint64_t broadcastBytes;
};

// =============================================================================
// P2P NODE
// =============================================================================
class P2PNode {
public:
    struct Config {
        std::string  listenHost             = "0.0.0.0";
        uint16_t     listenPort             = 30303;
        size_t       maxPeers               = 1000;
        size_t       ioThreads              = 4;
        uint32_t     connectTimeoutMs       = 5000;
        uint32_t     handshakeTimeoutMs     = 10000;
        uint32_t     pingIntervalSecs       = 30;
        uint32_t     peerTimeoutSecs        = 120;
        uint32_t     banDurationSecs        = 3600;
        uint32_t     maxMsgPerSecPeer       = 100;
        size_t       maxMsgSizeBytes        = 8 * 1024 * 1024;
        std::string  tlsCertPath;
        std::string  tlsKeyPath;
        std::string  tlsCAPath;
        std::string  tlsDhParamsPath;
        bool         requirePeerCert        = false;
        std::vector<std::string> bootstrapPeers;
        std::string  peerStorePath          = "data/peers.dat";
        uint32_t     reconnectIntervalSecs  = 60;
        uint32_t     maxReconnectAttempts   = 10;
        std::string  nodeId;
        uint32_t     protocolVersion        = 1;
        std::string  networkMagic           = "MEDOR";
    };

    explicit P2PNode(Config                          cfg,
                     std::shared_ptr<PeerManager>    peerMgr,
                     std::shared_ptr<MessageHandler> msgHandler);
    ~P2PNode();

    P2PNode(const P2PNode&)            = delete;
    P2PNode& operator=(const P2PNode&) = delete;

    void start();
    void stop();

    void connect  (const std::string& host, uint16_t port);
    void broadcast(std::shared_ptr<std::vector<uint8_t>> frame,
                    const std::string& excludeId = "");

    void onConnect   (std::function<void(const std::string&)> fn);
    void onDisconnect(std::function<void(const std::string&)> fn);
    void onError     (std::function<void(const std::string&,
                                          const std::string&)> fn);

    size_t         sessionCount() const noexcept;
    P2PNodeMetrics getMetrics()   const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace net


#include "block.h"
#include "transaction.h"
#include "net/peer_manager.h"
#include "net/message_handler.h"
#include "mempool/mempool.h"
#include "blockchain.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net {

// Forward declarations — defined in p2p_node.cpp
class Session;
class SessionRegistry;

// =============================================================================
// METRICS — exposed for Prometheus / monitoring integration
// =============================================================================
struct P2PNodeMetrics {
    uint64_t sessionsCreated;
    uint64_t sessionsDestroyed;
    uint64_t connectAttempts;
    uint64_t connectSuccess;
    uint64_t connectFailed;
    uint64_t acceptSuccess;
    uint64_t acceptRejected;
    uint64_t framesSent;
    uint64_t framesRecv;
    uint64_t bytesSent;
    uint64_t bytesRecv;
    uint64_t sendErrors;
    uint64_t recvErrors;
    uint64_t oversizedFrames;
    uint64_t tlsHandshakeFailed;
    uint64_t reconnectAttempts;
    uint64_t peersBanned;
    uint64_t peersEvicted;
    uint64_t broadcastsSent;
    uint64_t broadcastBytes;
};

// =============================================================================
// P2PNode
// Production-grade Boost.Asio P2P node with TLS, peer management,
// session registry, buffer pooling, per-session send queues,
// exponential backoff reconnect, structured error reporting,
// and full metrics.
// =============================================================================
class P2PNode {
public:
    struct Config {
        std::string  listenHost           = "0.0.0.0";
        uint16_t     listenPort           = 30303;
        size_t       maxInboundPeers      = 125;
        size_t       maxOutboundPeers     = 25;
        size_t       ioThreads            = 0; // 0 = auto (hardware_concurrency)
        uint32_t     connectTimeoutMs     = 5000;
        uint32_t     handshakeTimeoutMs   = 10000;
        uint32_t     pingIntervalSecs     = 30;
        uint32_t     peerTimeoutSecs      = 120;
        uint32_t     banDurationSecs      = 3600;
        uint32_t     maxMsgPerSecPeer     = 100;
        size_t       maxMsgSizeBytes      = 8 * 1024 * 1024; // 8 MB
        std::string  tlsCertFile;
        std::string  tlsKeyFile;
        std::string  tlsCAFile;
        std::string  tlsDHParamFile;      // path to dhparam.pem
        std::vector<std::string> bootstrapPeers;
        std::string  peerStoreFile        = "data/peers.dat";
        uint32_t     reconnectIntervalSec = 60;
        uint32_t     maxReconnectAttempts = 10;
        std::string  nodeId;
        uint32_t     protocolVersion      = 1;
        std::string  networkMagic         = "MEDOR";
        size_t       registryShards       = 16; // shards for broadcast lock reduction
    };

    using LogFn       = std::function<void(int, const std::string&)>;
    using BlockFn     = std::function<void(const Block&,
                                           const std::string& peerId)>;
    using TxFn        = std::function<void(const Transaction&,
                                           const std::string& peerId)>;
    using PeerEventFn = std::function<void(const std::string& peerId)>;
    using ErrorFn     = std::function<void(const std::string& peerId,
                                           const std::string& reason)>;

    explicit P2PNode(Config      cfg,
                     Blockchain& chain,
                     Mempool&    mempool);
    ~P2PNode();

    P2PNode(const P2PNode&)            = delete;
    P2PNode& operator=(const P2PNode&) = delete;

    // Lifecycle
    bool start();
    void stop();
    bool isRunning() const noexcept;

    // Callbacks — must be installed BEFORE start()
    void setLogger          (LogFn fn);
    void onBlockReceived    (BlockFn fn);
    void onTxReceived       (TxFn fn);
    void onPeerConnected    (PeerEventFn fn);
    void onPeerDisconnected (PeerEventFn fn);
    void onError            (ErrorFn fn);

    // Broadcast — non-blocking, posted to io_context
    void broadcastBlock      (const Block& block);
    void broadcastTransaction(const Transaction& tx);

    // Queries
    size_t         connectedPeers() const noexcept;
    std::string    nodeId()         const noexcept;
    P2PNodeMetrics getMetrics()     const noexcept;

    // Direct peer management
    void connectToPeer   (const std::string& host, uint16_t port);
    void disconnectPeer  (const std::string& peerId);

private:
    Config      cfg_;
    Blockchain& chain_;
    Mempool&    mempool_;

    boost::asio::io_context  ioc_;
    boost::asio::ssl::context sslCtx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer      pingTimer_;
    boost::asio::steady_timer      reconnectTimer_;
    boost::asio::steady_timer      peerPersistTimer_;
    boost::asio::steady_timer      metricsTimer_;

    std::vector<std::thread> ioThreads_;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> work_;

    std::shared_ptr<PeerManager>    peerMgr_;
    std::shared_ptr<MessageHandler> msgHandler_;
    std::shared_ptr<SessionRegistry> registry_;

    mutable std::mutex callbackMu_;
    LogFn       logFn_;
    BlockFn     blockFn_;
    TxFn        txFn_;
    PeerEventFn peerConnFn_;
    PeerEventFn peerDiscFn_;
    ErrorFn     errorFn_;

    std::atomic<bool> running_{false};

    // Internal helpers
    void slog(int level, const std::string& msg);
    bool initTLS();
    void doAccept();
    void schedulePing();
    void scheduleReconnect();
    void schedulePeerPersist();
    void scheduleMetricsDump();
    void pingAllPeers();
    void reconnectBootstrap();
    void loadPersistedPeers();
    void persistPeers();

    // Callbacks wired into Session
    void handlePeerConnect   (const std::string& peerId);
    void handlePeerDisconnect(const std::string& peerId);
    void handleError         (const std::string& peerId,
                              const std::string& reason);
};

} // namespace net
