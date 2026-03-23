#include "net/p2p_node.h"
#include "net/message_handler.h"

#include <boost/asio/ssl.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace net {

using tcp      = boost::asio::ip::tcp;
using ssl_sock = boost::asio::ssl::stream<tcp::socket>;

// =============================================================================
// METRICS
// =============================================================================
struct P2PMetrics {
    std::atomic<uint64_t> sessionsCreated{0};
    std::atomic<uint64_t> sessionsDestroyed{0};
    std::atomic<uint64_t> connectAttempts{0};
    std::atomic<uint64_t> connectSuccess{0};
    std::atomic<uint64_t> connectFailed{0};
    std::atomic<uint64_t> acceptSuccess{0};
    std::atomic<uint64_t> acceptRejected{0};
    std::atomic<uint64_t> framesSent{0};
    std::atomic<uint64_t> framesRecv{0};
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<uint64_t> bytesRecv{0};
    std::atomic<uint64_t> sendErrors{0};
    std::atomic<uint64_t> recvErrors{0};
    std::atomic<uint64_t> oversizedFrames{0};
    std::atomic<uint64_t> tlsHandshakeFailed{0};
    std::atomic<uint64_t> reconnectAttempts{0};
    std::atomic<uint64_t> peersBanned{0};
    std::atomic<uint64_t> peersEvicted{0};
    std::atomic<uint64_t> broadcastsSent{0};
    std::atomic<uint64_t> broadcastBytes{0};

    void dump(std::ostream& os) const {
        os << "[P2PMetrics]"
           << " sessions=" << sessionsCreated.load()
           << " active="   << (sessionsCreated.load()
                               - sessionsDestroyed.load())
           << " sent="     << framesSent.load()
           << " recv="     << framesRecv.load()
           << " txBytes="  << bytesSent.load()
           << " rxBytes="  << bytesRecv.load()
           << " sendErr="  << sendErrors.load()
           << " recvErr="  << recvErrors.load()
           << " tlsFail="  << tlsHandshakeFailed.load()
           << " banned="   << peersBanned.load()
           << " evicted="  << peersEvicted.load()
           << "\n";
    }
};

static P2PMetrics g_metrics;

P2PNodeMetrics P2PNode::getMetrics() const noexcept {
    return {
        g_metrics.sessionsCreated.load(std::memory_order_relaxed),
        g_metrics.sessionsDestroyed.load(std::memory_order_relaxed),
        g_metrics.connectAttempts.load(std::memory_order_relaxed),
        g_metrics.connectSuccess.load(std::memory_order_relaxed),
        g_metrics.connectFailed.load(std::memory_order_relaxed),
        g_metrics.acceptSuccess.load(std::memory_order_relaxed),
        g_metrics.acceptRejected.load(std::memory_order_relaxed),
        g_metrics.framesSent.load(std::memory_order_relaxed),
        g_metrics.framesRecv.load(std::memory_order_relaxed),
        g_metrics.bytesSent.load(std::memory_order_relaxed),
        g_metrics.bytesRecv.load(std::memory_order_relaxed),
        g_metrics.sendErrors.load(std::memory_order_relaxed),
        g_metrics.recvErrors.load(std::memory_order_relaxed),
        g_metrics.oversizedFrames.load(std::memory_order_relaxed),
        g_metrics.tlsHandshakeFailed.load(std::memory_order_relaxed),
        g_metrics.reconnectAttempts.load(std::memory_order_relaxed),
        g_metrics.peersBanned.load(std::memory_order_relaxed),
        g_metrics.peersEvicted.load(std::memory_order_relaxed),
        g_metrics.broadcastsSent.load(std::memory_order_relaxed),
        g_metrics.broadcastBytes.load(std::memory_order_relaxed)
    };
}

// =============================================================================
// BUFFER POOL
// =============================================================================
class BufferPool {
public:
    static constexpr size_t BUFFER_SIZE = 64 * 1024;

    std::shared_ptr<std::vector<uint8_t>> acquire() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!pool_.empty()) {
            auto buf = pool_.back();
            pool_.pop_back();
            buf->clear();
            return buf;
        }
        return std::make_shared<std::vector<uint8_t>>();
    }

    void release(std::shared_ptr<std::vector<uint8_t>> buf) {
        if (!buf) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (pool_.size() < MAX_POOL_SIZE) {
            buf->clear();
            pool_.push_back(std::move(buf));
        }
    }

private:
    static constexpr size_t MAX_POOL_SIZE = 512;
    std::mutex mu_;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> pool_;
};

static BufferPool g_bufferPool;

// =============================================================================
// RECONNECT TRACKER
// =============================================================================
struct ReconnectEntry {
    uint32_t attempts    = 0;
    uint64_t nextRetryAt = 0;
    uint32_t delayMs     = 1000;

    void recordFailure() {
        ++attempts;
        delayMs = std::min(delayMs * 2u, uint32_t(60'000));
        nextRetryAt = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()
                    .time_since_epoch()).count())
            + delayMs;
    }

    bool readyToRetry() const {
        uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()
                    .time_since_epoch()).count());
        return now >= nextRetryAt;
    }

    void reset() { attempts = 0; delayMs = 1000; nextRetryAt = 0; }
};

class ReconnectTracker {
public:
    bool shouldRetry(const std::string& key, uint32_t maxAttempts) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& e = entries_[key];
        if (e.attempts >= maxAttempts) return false;
        return e.readyToRetry();
    }

    void recordFailure(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        entries_[key].recordFailure();
    }

    void recordSuccess(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        entries_[key].reset();
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, ReconnectEntry> entries_;
};

static ReconnectTracker g_reconnect;

// =============================================================================
// SESSION
// =============================================================================
class Session : public std::enable_shared_from_this<Session> {
public:
    using ErrorFn = std::function<void(const std::string& peerId,
                                       const std::string& reason)>;

    Session(boost::asio::io_context&         ioc,
            boost::asio::ssl::context&       sslCtx,
            std::shared_ptr<PeerManager>     peerMgr,
            std::shared_ptr<MessageHandler>  msgHandler,
            const P2PNode::Config&           cfg,
            bool                             isInbound,
            std::function<void(const std::string&)> onConnect,
            std::function<void(const std::string&)> onDisconnect,
            ErrorFn                          onError)
        : socket_(ioc, sslCtx)
        , resolver_(ioc)
        , peerMgr_(std::move(peerMgr))
        , msgHandler_(std::move(msgHandler))
        , cfg_(cfg)
        , isInbound_(isInbound)
        , onConnect_(std::move(onConnect))
        , onDisconnect_(std::move(onDisconnect))
        , onError_(std::move(onError))
    {
        g_metrics.sessionsCreated.fetch_add(1, std::memory_order_relaxed);
        readBuf_ = g_bufferPool.acquire();
    }

    ~Session() {
        g_metrics.sessionsDestroyed.fetch_add(1, std::memory_order_relaxed);
        g_bufferPool.release(readBuf_);
        disconnect("session destroyed");
    }

    ssl_sock&   socket() { return socket_; }
    std::string peerId() const { return peerId_; }

    void send(std::shared_ptr<std::vector<uint8_t>> frame) {
        if (cancelled_.load()) return;
        boost::asio::post(
            socket_.get_executor(),
            [this, self = shared_from_this(),
             frame = std::move(frame)]() {
                bool idle = sendQueue_.empty();
                sendQueue_.push_back(frame);
                if (idle) doSend();
            });
    }

    void startInbound() {
        auto self = shared_from_this();
        socket_.async_handshake(
            boost::asio::ssl::stream_base::server,
            [this, self](const boost::system::error_code& ec) {
                if (cancelled_.load()) return;
                if (ec) {
                    g_metrics.tlsHandshakeFailed.fetch_add(1,
                        std::memory_order_relaxed);
                    reportError("TLS inbound: " + ec.message());
                    disconnect("TLS failed");
                    return;
                }
                doProtocolHandshake();
            });
    }

    void startOutbound(const std::string& host, uint16_t port) {
        if (cancelled_.load()) return;
        host_ = host;
        port_ = port;
        g_metrics.connectAttempts.fetch_add(1, std::memory_order_relaxed);
        auto self = shared_from_this();
        resolver_.async_resolve(
            host, std::to_string(port),
            [this, self, host, port](
                const boost::system::error_code& ec,
                tcp::resolver::results_type results)
            {
                if (cancelled_.load()) return;
                if (ec) {
                    g_metrics.connectFailed.fetch_add(1,
                        std::memory_order_relaxed);
                    g_reconnect.recordFailure(
                        host + ":" + std::to_string(port));
                    reportError("resolve: " + ec.message());
                    return;
                }
                boost::asio::async_connect(
                    socket_.lowest_layer(), results,
                    [this, self, host, port](
                        const boost::system::error_code& ec2,
                        const tcp::endpoint&)
                    {
                        if (cancelled_.load()) return;
                        if (ec2) {
                            g_metrics.connectFailed.fetch_add(1,
                                std::memory_order_relaxed);
                            g_reconnect.recordFailure(
                                host + ":" + std::to_string(port));
                            reportError("connect: " + ec2.message());
                            return;
                        }
                        socket_.async_handshake(
                            boost::asio::ssl::stream_base::client,
                            [this, self, host, port](
                                const boost::system::error_code& ec3)
                            {
                                if (cancelled_.load()) return;
                                if (ec3) {
                                    g_metrics.tlsHandshakeFailed
                                        .fetch_add(1,
                                        std::memory_order_relaxed);
                                    g_metrics.connectFailed.fetch_add(1,
                                        std::memory_order_relaxed);
                                    g_reconnect.recordFailure(
                                        host + ":" + std::to_string(port));
                                    reportError("TLS outbound: "
                                                + ec3.message());
                                    return;
                                }
                                g_metrics.connectSuccess.fetch_add(1,
                                    std::memory_order_relaxed);
                                g_reconnect.recordSuccess(
                                    host + ":" + std::to_string(port));
                                doProtocolHandshake();
                            });
                    });
            });
    }

private:
    ssl_sock                                socket_;
    tcp::resolver                           resolver_;
    std::shared_ptr<PeerManager>            peerMgr_;
    std::shared_ptr<MessageHandler>         msgHandler_;
    const P2PNode::Config&                  cfg_;
    bool                                    isInbound_;
    std::atomic<bool>                       cancelled_{false};
    std::string                             peerId_;
    std::string                             host_;
    uint16_t                                port_ = 0;

    std::shared_ptr<std::vector<uint8_t>>   readBuf_;
    std::array<uint8_t, FRAME_HEADER_SIZE>  hdrBuf_{};

    std::vector<std::shared_ptr<
        std::vector<uint8_t>>>              sendQueue_;

    std::function<void(const std::string&)> onConnect_;
    std::function<void(const std::string&)> onDisconnect_;
    ErrorFn                                 onError_;

    static std::atomic<uint64_t> s_counter;

    static std::string makePeerId(const std::string& host,
                                   uint16_t port) {
        return host + ":" + std::to_string(port) + "#"
             + std::to_string(s_counter.fetch_add(1,
                   std::memory_order_relaxed));
    }

    void reportError(const std::string& reason) {
        if (onError_) {
            try { onError_(peerId_, reason); } catch (...) {}
        }
    }

    void doProtocolHandshake() {
        if (cancelled_.load()) return;
        try {
            auto ep = socket_.lowest_layer().remote_endpoint();
            host_   = ep.address().to_string();
            port_   = ep.port();
        } catch (...) {
            if (host_.empty()) host_ = "unknown";
        }

        peerId_ = makePeerId(host_, port_);

        PeerInfo info;
        info.id         = peerId_;
        info.address    = host_;
        info.port       = port_;
        info.isInbound  = isInbound_;
        uint64_t nowS   = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now()
                    .time_since_epoch()).count());
        info.connectedAt = nowS;
        info.lastSeenAt  = nowS;

        if (!peerMgr_->addPeer(info)) {
            disconnect("peer limit or duplicate");
            return;
        }

        if (onConnect_) {
            try { onConnect_(peerId_); } catch (...) {}
        }

        auto hs = std::make_shared<std::vector<uint8_t>>(
                      msgHandler_->buildHandshake());
        send(hs);
        readHeader();
    }

    void doSend() {
        if (cancelled_.load() || sendQueue_.empty()) return;
        auto frame = sendQueue_.front();
        auto self  = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(*frame),
            [this, self, frame](
                const boost::system::error_code& ec, size_t n)
            {
                if (cancelled_.load()) return;
                if (ec) {
                    g_metrics.sendErrors.fetch_add(1,
                        std::memory_order_relaxed);
                    reportError("send: " + ec.message());
                    disconnect("send failed");
                    return;
                }
                g_metrics.framesSent.fetch_add(1,
                    std::memory_order_relaxed);
                g_metrics.bytesSent.fetch_add(n,
                    std::memory_order_relaxed);
                sendQueue_.erase(sendQueue_.begin());
                if (!sendQueue_.empty()) doSend();
            });
    }

    void readHeader() {
        if (cancelled_.load()) return;
        auto self = shared_from_this();
        boost::asio::async_read(
            socket_,
            boost::asio::buffer(hdrBuf_),
            [this, self](
                const boost::system::error_code& ec, size_t)
            {
                if (cancelled_.load()) return;
                if (ec) {
                    g_metrics.recvErrors.fetch_add(1,
                        std::memory_order_relaxed);
                    if (ec != boost::asio::error::eof &&
                        ec != boost::asio::error::operation_aborted)
                        reportError("read header: " + ec.message());
                    disconnect("recv failed");
                    return;
                }
                uint32_t payLen =
                    (static_cast<uint32_t>(hdrBuf_[5]) << 24) |
                    (static_cast<uint32_t>(hdrBuf_[6]) << 16) |
                    (static_cast<uint32_t>(hdrBuf_[7]) <<  8) |
                     static_cast<uint32_t>(hdrBuf_[8]);

                if (payLen > cfg_.maxMsgSizeBytes) {
                    g_metrics.oversizedFrames.fetch_add(1,
                        std::memory_order_relaxed);
                    peerMgr_->penalizePeer(peerId_, 20.0);
                    reportError("oversized frame: "
                                + std::to_string(payLen));
                    disconnect("oversized frame");
                    return;
                }
                readPayload(payLen);
            });
    }

    void readPayload(uint32_t payLen) {
        if (cancelled_.load()) return;
        auto self = shared_from_this();
        readBuf_->resize(FRAME_HEADER_SIZE + payLen);
        std::copy(hdrBuf_.begin(), hdrBuf_.end(), readBuf_->begin());

        boost::asio::async_read(
            socket_,
            boost::asio::buffer(
                readBuf_->data() + FRAME_HEADER_SIZE, payLen),
            [this, self](
                const boost::system::error_code& ec, size_t n)
            {
                if (cancelled_.load()) return;
                if (ec) {
                    g_metrics.recvErrors.fetch_add(1,
                        std::memory_order_relaxed);
                    reportError("read payload: " + ec.message());
                    disconnect("payload read failed");
                    return;
                }
                g_metrics.framesRecv.fetch_add(1,
                    std::memory_order_relaxed);
                g_metrics.bytesRecv.fetch_add(
                    FRAME_HEADER_SIZE + n,
                    std::memory_order_relaxed);

                try {
                    msgHandler_->handleRaw(peerId_, *readBuf_);
                } catch (const std::exception& e) {
                    peerMgr_->penalizePeer(peerId_, 5.0);
                    reportError("handleRaw: " + std::string(e.what()));
                } catch (...) {
                    peerMgr_->penalizePeer(peerId_, 5.0);
                    reportError("handleRaw unknown exception");
                }
                readHeader();
            });
    }

    void disconnect(const std::string& reason) {
        if (cancelled_.exchange(true)) return;
        boost::system::error_code ec;
        socket_.lowest_layer().cancel(ec);
        socket_.lowest_layer().shutdown(
            tcp::socket::shutdown_both, ec);
        socket_.lowest_layer().close(ec);
        if (!peerId_.empty()) {
            peerMgr_->removePeer(peerId_);
            if (onDisconnect_) {
                try { onDisconnect_(peerId_); } catch (...) {}
            }
        }
    }
};

std::atomic<uint64_t> Session::s_counter{0};

// =============================================================================
// SESSION REGISTRY
// Sharded registry to reduce lock contention on broadcast.
// The registry is split into N shards each with its own mutex.
// Broadcast iterates all shards and sends to each session.
// =============================================================================
class SessionRegistry {
public:
    static constexpr size_t SHARD_COUNT = 32;

    void add(const std::string& id,
              std::shared_ptr<Session> session) {
        auto& shard = shardFor(id);
        std::lock_guard<std::mutex> lk(shard.mu);
        shard.sessions[id] = std::move(session);
    }

    void remove(const std::string& id) {
        auto& shard = shardFor(id);
        std::lock_guard<std::mutex> lk(shard.mu);
        shard.sessions.erase(id);
    }

    std::shared_ptr<Session> get(const std::string& id) {
        auto& shard = shardFor(id);
        std::lock_guard<std::mutex> lk(shard.mu);
        auto it = shard.sessions.find(id);
        if (it == shard.sessions.end()) return nullptr;
        return it->second;
    }

    void broadcast(std::shared_ptr<std::vector<uint8_t>> frame,
                    const std::string& excludeId = "") {
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lk(shard.mu);
            for (auto& [id, session] : shard.sessions) {
                if (id == excludeId) continue;
                if (session) session->send(frame);
            }
        }
        g_metrics.broadcastsSent.fetch_add(1,
            std::memory_order_relaxed);
        g_metrics.broadcastBytes.fetch_add(frame->size(),
            std::memory_order_relaxed);
    }

    size_t count() const {
        size_t n = 0;
        for (const auto& shard : shards_) {
            std::lock_guard<std::mutex> lk(shard.mu);
            n += shard.sessions.size();
        }
        return n;
    }

private:
    struct Shard {
        mutable std::mutex mu;
        std::unordered_map<std::string,
                           std::shared_ptr<Session>> sessions;
    };

    std::array<Shard, SHARD_COUNT> shards_;

    Shard& shardFor(const std::string& id) {
        return shards_[std::hash<std::string>{}(id) % SHARD_COUNT];
    }
};

// =============================================================================
// P2P NODE IMPLEMENTATION
// =============================================================================
struct P2PNode::Impl {
    Config                           cfg;
    boost::asio::io_context          ioc;
    boost::asio::ssl::context        sslCtx{
        boost::asio::ssl::context::tls};
    tcp::acceptor                    acceptor{ioc};
    std::vector<std::thread>         ioThreads;
    std::shared_ptr<PeerManager>     peerMgr;
    std::shared_ptr<MessageHandler>  msgHandler;
    SessionRegistry                  registry;
    std::atomic<bool>                running{false};

    std::function<void(const std::string&)> onConnectCb;
    std::function<void(const std::string&)> onDisconnectCb;
    std::function<void(const std::string&,
                        const std::string&)>  onErrorCb;

    std::mutex cbMu;

    void setupTLS() {
        sslCtx.set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2            |
            boost::asio::ssl::context::no_sslv3            |
            boost::asio::ssl::context::no_tlsv1            |
            boost::asio::ssl::context::no_tlsv1_1          |
            boost::asio::ssl::context::single_dh_use);

        if (!cfg.tlsCertPath.empty())
            sslCtx.use_certificate_chain_file(cfg.tlsCertPath);
        if (!cfg.tlsKeyPath.empty())
            sslCtx.use_private_key_file(cfg.tlsKeyPath,
                boost::asio::ssl::context::pem);
        if (!cfg.tlsDhParamsPath.empty())
            sslCtx.use_tmp_dh_file(cfg.tlsDhParamsPath);

        sslCtx.set_verify_mode(
            cfg.requirePeerCert
            ? boost::asio::ssl::verify_peer
            : boost::asio::ssl::verify_none);
    }

    std::shared_ptr<Session> makeSession(bool isInbound) {
        return std::make_shared<Session>(
            ioc, sslCtx, peerMgr, msgHandler, cfg,
            isInbound,
            [this](const std::string& id) {
                registry.add(id, registry.get(id));
                std::lock_guard<std::mutex> lk(cbMu);
                if (onConnectCb) {
                    try { onConnectCb(id); } catch (...) {}
                }
            },
            [this](const std::string& id) {
                registry.remove(id);
                std::lock_guard<std::mutex> lk(cbMu);
                if (onDisconnectCb) {
                    try { onDisconnectCb(id); } catch (...) {}
                }
            },
            [this](const std::string& id,
                    const std::string& reason) {
                std::lock_guard<std::mutex> lk(cbMu);
                if (onErrorCb) {
                    try { onErrorCb(id, reason); } catch (...) {}
                }
            });
    }

    void doAccept() {
        auto session = makeSession(true);
        acceptor.async_accept(
            session->socket().lowest_layer(),
            [this, session](const boost::system::error_code& ec) {
                if (!running.load()) return;
                if (!ec) {
                    if (peerMgr->peerCount() < cfg.maxPeers) {
                        g_metrics.acceptSuccess.fetch_add(1,
                            std::memory_order_relaxed);
                        session->startInbound();
                    } else {
                        g_metrics.acceptRejected.fetch_add(1,
                            std::memory_order_relaxed);
                    }
                }
                doAccept();
            });
    }
};

// =============================================================================
// P2P NODE PUBLIC API
// =============================================================================
P2PNode::P2PNode(Config cfg,
                  std::shared_ptr<PeerManager>    peerMgr,
                  std::shared_ptr<MessageHandler> msgHandler)
    : impl_(std::make_unique<Impl>())
{
    impl_->cfg        = std::move(cfg);
    impl_->peerMgr    = std::move(peerMgr);
    impl_->msgHandler = std::move(msgHandler);
}

P2PNode::~P2PNode() { stop(); }

void P2PNode::start() {
    if (impl_->running.exchange(true)) return;

    impl_->setupTLS();

    tcp::endpoint ep(tcp::v4(),
        static_cast<uint16_t>(impl_->cfg.listenPort));
    impl_->acceptor.open(ep.protocol());
    impl_->acceptor.set_option(
        boost::asio::socket_base::reuse_address(true));
    impl_->acceptor.bind(ep);
    impl_->acceptor.listen(
        boost::asio::socket_base::max_listen_connections);

    impl_->doAccept();

    size_t threads = std::max(size_t(1), impl_->cfg.ioThreads);
    impl_->ioThreads.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
        impl_->ioThreads.emplace_back([this]() {
            impl_->ioc.run();
        });
    }

    std::cout << "[P2PNode] Listening on port "
              << impl_->cfg.listenPort
              << " threads=" << threads << "\n";
}

void P2PNode::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->acceptor.close();
    impl_->ioc.stop();
    for (auto& t : impl_->ioThreads)
        if (t.joinable()) t.join();
    impl_->ioThreads.clear();
    std::cout << "[P2PNode] Stopped\n";
}

void P2PNode::connect(const std::string& host, uint16_t port) {
    if (!impl_->running.load()) return;
    auto session = impl_->makeSession(false);
    std::string key = host + ":" + std::to_string(port);
    if (!g_reconnect.shouldRetry(key, impl_->cfg.maxReconnectAttempts))
        return;
    g_metrics.reconnectAttempts.fetch_add(1, std::memory_order_relaxed);
    session->startOutbound(host, port);
}

void P2PNode::broadcast(std::shared_ptr<std::vector<uint8_t>> frame,
                          const std::string& excludeId) {
    impl_->registry.broadcast(std::move(frame), excludeId);
}

void P2PNode::onConnect(std::function<void(const std::string&)> fn) {
    std::lock_guard<std::mutex> lk(impl_->cbMu);
    impl_->onConnectCb = std::move(fn);
}

void P2PNode::onDisconnect(std::function<void(const std::string&)> fn) {
    std::lock_guard<std::mutex> lk(impl_->cbMu);
    impl_->onDisconnectCb = std::move(fn);
}

void P2PNode::onError(std::function<void(const std::string&,
                                          const std::string&)> fn) {
    std::lock_guard<std::mutex> lk(impl_->cbMu);
    impl_->onErrorCb = std::move(fn);
}

size_t P2PNode::sessionCount() const noexcept {
    return impl_->registry.count();
}

} // namespace net
