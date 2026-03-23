#pragma once

#include “net/peer_manager.h”
#include “net/message_handler.h”
#include “block.h”
#include “transaction.h”
#include “mempool/mempool.h”
#include “blockchain.h”

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
// P2PNode
// =============================================================================
class P2PNode {
public:
struct Config {
std::string  listenHost            = “0.0.0.0”;
uint16_t     listenPort            = 30303;
size_t       maxPeers              = 1000;
size_t       maxInboundPeers       = 125;
size_t       maxOutboundPeers      = 25;
size_t       ioThreads             = 4;
uint32_t     connectTimeoutMs      = 5000;
uint32_t     handshakeTimeoutMs    = 10000;
uint32_t     pingIntervalSecs      = 30;
uint32_t     peerTimeoutSecs       = 120;
uint32_t     banDurationSecs       = 3600;
uint32_t     maxMsgPerSecPeer      = 100;
size_t       maxMsgSizeBytes       = 8 * 1024 * 1024;
std::string  tlsCertPath;
std::string  tlsKeyPath;
std::string  tlsCAPath;
std::string  tlsDhParamsPath;
bool         requirePeerCert       = false;
std::vector<std::string> bootstrapPeers;
std::string  peerStorePath         = “data/peers.dat”;
uint32_t     reconnectIntervalSecs = 60;
uint32_t     maxReconnectAttempts  = 10;
std::string  nodeId;
uint32_t     protocolVersion       = 1;
std::string  networkMagic          = “MEDOR”;
size_t       registryShards        = 16;
};

```
using LogFn       = std::function<void(int, const std::string&)>;
using BlockFn     = std::function<void(const Block&,
                                       const std::string& peerId)>;
using TxFn        = std::function<void(const Transaction&,
                                       const std::string& peerId)>;
using PeerEventFn = std::function<void(const std::string& peerId)>;
using ErrorFn     = std::function<void(const std::string& peerId,
                                       const std::string& reason)>;

// Primary constructor — pimpl pattern
explicit P2PNode(Config                          cfg,
                 std::shared_ptr<PeerManager>    peerMgr,
                 std::shared_ptr<MessageHandler> msgHandler);

// Convenience constructor for node.cpp / higher-level callers
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
void setLogger           (LogFn fn);
void onBlockReceived     (BlockFn fn);
void onTxReceived        (TxFn fn);
void onConnect           (PeerEventFn fn);
void onDisconnect        (PeerEventFn fn);
void onError             (ErrorFn fn);

// Aliases kept for backward-compatibility with existing call-sites
void onPeerConnected     (PeerEventFn fn);   // same as onConnect
void onPeerDisconnected  (PeerEventFn fn);   // same as onDisconnect

// Broadcast
void broadcastBlock      (const Block& block);
void broadcastTransaction(const Transaction& tx);
void broadcast           (std::shared_ptr<std::vector<uint8_t>> frame,
                          const std::string& excludeId = "");

// Peer management
void connect        (const std::string& host, uint16_t port);
void connectToPeer  (const std::string& host, uint16_t port); // alias
void disconnectPeer (const std::string& peerId);

// Queries
size_t         sessionCount()   const noexcept;
size_t         connectedPeers() const noexcept;  // alias for sessionCount
std::string    nodeId()         const noexcept;
P2PNodeMetrics getMetrics()     const noexcept;
```

private:
struct Impl;
std::unique_ptr<Impl> impl_;
};

} // namespace net
