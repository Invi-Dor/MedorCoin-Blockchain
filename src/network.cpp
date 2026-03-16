#include "network.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

static constexpr int LISTEN_BACKLOG = 128;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

Network::Network(Config cfg)
    : cfg_(std::move(cfg))
    , connPool_(ConnectionPool::Config{
          cfg_.maxConnsPerPeer,
          cfg_.connectTimeoutMs,
          cfg_.peerTimeoutSecs })
    , inboundPool_ (cfg_.inboundWorkers,  cfg_.inboundQueueDepth)
    , outboundPool_(cfg_.outboundWorkers, cfg_.outboundQueueDepth)
{
    peers_.reserve(cfg_.maxPeers);
}

Network::~Network() { stop(); }

// ─────────────────────────────────────────────────────────────────────────────
// Time helpers
// ─────────────────────────────────────────────────────────────────────────────

uint64_t Network::nowSecs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t Network::nowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

bool Network::start() noexcept
{
    if (listenerRunning_.load()) return true;

    if (!bindListener()) return false;

    listenerRunning_.store(true);
    listenerThread_ = std::thread([this]() { runListener(); });

    healthRunning_.store(true);
    healthThread_ = std::thread([this]() { runHealthCheck(); });

    pingRunning_.store(true);
    pingThread_ = std::thread([this]() { runPingLoop(); });

    logger_.log(0, "Network started on port " + std::to_string(cfg_.listenPort));
    return true;
}

void Network::stop() noexcept
{
    listenerRunning_.store(false);
    healthRunning_.store(false);
    pingRunning_.store(false);

    if (listenerFd_ >= 0) {
        ::shutdown(listenerFd_, SHUT_RDWR);
        ::close(listenerFd_);
        listenerFd_ = -1;
    }

    if (listenerThread_.joinable()) listenerThread_.join();
    if (healthThread_.joinable())   healthThread_.join();
    if (pingThread_.joinable())     pingThread_.join();

    connPool_.clear();
    logger_.log(0, "Network stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
// Callbacks
// ─────────────────────────────────────────────────────────────────────────────

void Network::setLogSink(AsyncLogger::SinkFn fn) noexcept
    { logger_.setSink(std::move(fn)); }

void Network::onBlockReceived(BlockReceivedFn fn) noexcept
    { std::lock_guard<std::mutex> l(callbackMutex_); onBlockReceivedFn_ = std::move(fn); }

void Network::onTransactionReceived(TransactionReceivedFn fn) noexcept
    { std::lock_guard<std::mutex> l(callbackMutex_); onTransactionReceivedFn_ = std::move(fn); }

void Network::onPeerConnected(PeerConnectedFn fn) noexcept
    { std::lock_guard<std::mutex> l(callbackMutex_); onPeerConnectedFn_ = std::move(fn); }

void Network::onPeerDisconnected(PeerDisconnectedFn fn) noexcept
    { std::lock_guard<std::mutex> l(callbackMutex_); onPeerDisconnectedFn_ = std::move(fn); }

// ─────────────────────────────────────────────────────────────────────────────
// Socket utilities
// ─────────────────────────────────────────────────────────────────────────────

bool Network::setSocketTimeouts(int fd,
                                 uint32_t sendMs,
                                 uint32_t recvMs) noexcept
{
    struct timeval tv;
    tv.tv_sec  = sendMs / 1000;
    tv.tv_usec = (sendMs % 1000) * 1000;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
        return false;
    tv.tv_sec  = recvMs / 1000;
    tv.tv_usec = (recvMs % 1000) * 1000;
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

bool Network::sendRaw(int fd,
                       const std::vector<uint8_t> &data,
                       uint32_t timeoutMs) noexcept
{
    struct timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    size_t       remaining = data.size();
    const char  *ptr       = reinterpret_cast<const char *>(data.data());

    while (remaining > 0) {
        ssize_t n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n <= 0) return false;
        ptr       += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

bool Network::recvFrame(int fd, codec::Frame &frameOut) noexcept
{
    // Accumulate bytes until we can decode a complete frame
    std::vector<uint8_t> buf;
    buf.reserve(codec::HEADER_BYTES + 256);

    // Read header first
    buf.resize(codec::HEADER_BYTES);
    size_t received = 0;
    while (received < codec::HEADER_BYTES) {
        ssize_t n = ::recv(fd, buf.data() + received,
                           codec::HEADER_BYTES - received, 0);
        if (n <= 0) return false;
        received += static_cast<size_t>(n);
    }

    // Decode payload length from header (bytes 5–8 inclusive)
    const uint32_t payLen =
        (static_cast<uint32_t>(buf[5]) << 24) |
        (static_cast<uint32_t>(buf[6]) << 16) |
        (static_cast<uint32_t>(buf[7]) <<  8) |
         static_cast<uint32_t>(buf[8]);

    if (payLen > cfg_.maxMsgPerSecPerPeer * codec::MAX_FRAME_BYTES) {
        // Oversized frame — close connection immediately
        return false;
    }

    buf.resize(codec::HEADER_BYTES + payLen);
    while (received < codec::HEADER_BYTES + payLen) {
        ssize_t n = ::recv(fd, buf.data() + received,
                           buf.size() - received, 0);
        if (n <= 0) return false;
        received += static_cast<size_t>(n);
    }

    size_t consumed = 0;
    try {
        auto opt = codec::decodeFrame(buf.data(), buf.size(), consumed);
        if (!opt) return false;
        frameOut = std::move(*opt);
        return true;
    } catch (const std::exception &e) {
        logger_.log(2, std::string("recvFrame: malformed frame: ") + e.what());
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Rate limiting — per peer, per second
// ─────────────────────────────────────────────────────────────────────────────

bool Network::checkRateLimit(PeerInfo &peer) noexcept
{
    const uint64_t sec = nowSecs();
    if (peer.rateLimitEpoch != sec) {
        peer.rateLimitEpoch   = sec;
        peer.msgThisSecond    = 0;
    }
    if (peer.msgThisSecond >= cfg_.maxMsgPerSecPerPeer) {
        logger_.log(1, "checkRateLimit: peer '" + peer.address
                       + "' exceeded " + std::to_string(cfg_.maxMsgPerSecPerPeer)
                       + " msg/s — dropping frame");
        return false;
    }
    ++peer.msgThisSecond;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// deliverToPeer — persistent connection pool + retry + back-off
// ─────────────────────────────────────────────────────────────────────────────

bool Network::deliverToPeer(PeerInfo                   &peer,
                              codec::MessageType          type,
                              const std::vector<uint8_t> &payload) noexcept
{
    if (peer.isBanned) {
        logger_.log(1, "deliverToPeer: peer '" + peer.address
                       + "' is banned — skipping");
        return false;
    }

    codec::Frame frame;
    frame.type    = type;
    frame.payload = payload;
    const auto encoded = codec::encodeFrame(frame);

    uint32_t delayMs = cfg_.retryBaseDelayMs;

    for (uint32_t attempt = 0; attempt <= cfg_.maxSendRetries; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            delayMs *= 2;   // exponential back-off
        }

        int fd = connPool_.acquire(peer.address, peer.port);
        if (fd < 0) {
            logger_.log(1, "deliverToPeer: cannot acquire connection to '"
                           + peer.address + "' (attempt "
                           + std::to_string(attempt + 1) + ")");
            continue;
        }

        setSocketTimeouts(fd, cfg_.sendTimeoutMs, cfg_.recvTimeoutMs);
        const bool sent = sendRaw(fd, encoded, cfg_.sendTimeoutMs);
        connPool_.release(peer.address, peer.port, fd, sent);

        if (sent) {
            peer.messagesSent++;
            peer.lastSeenAt  = nowSecs();
            peer.isReachable = true;
            return true;
        }

        logger_.log(1, "deliverToPeer: send failed to '"
                       + peer.address + "' (attempt "
                       + std::to_string(attempt + 1) + "/"
                       + std::to_string(cfg_.maxSendRetries + 1) + ")");
    }

    peer.isReachable = false;
    logger_.log(2, "deliverToPeer: peer '" + peer.address
                   + "' unreachable after "
                   + std::to_string(cfg_.maxSendRetries + 1) + " attempts");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Listener
// ─────────────────────────────────────────────────────────────────────────────

bool Network::bindListener() noexcept
{
    listenerFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenerFd_ < 0) {
        logger_.log(2, "bindListener: socket() failed — "
                       + std::string(std::strerror(errno)));
        return false;
    }

    int one = 1;
    ::setsockopt(listenerFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(listenerFd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(cfg_.listenPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(listenerFd_,
               reinterpret_cast<struct sockaddr *>(&addr),
               sizeof(addr)) != 0)
    {
        logger_.log(2, "bindListener: bind() failed on port "
                       + std::to_string(cfg_.listenPort)
                       + " — " + std::strerror(errno));
        ::close(listenerFd_);
        listenerFd_ = -1;
        return false;
    }

    if (::listen(listenerFd_, LISTEN_BACKLOG) != 0) {
        logger_.log(2, "bindListener: listen() failed — "
                       + std::string(std::strerror(errno)));
        ::close(listenerFd_);
        listenerFd_ = -1;
        return false;
    }
    return true;
}

void Network::runListener() noexcept
{
    while (listenerRunning_.load()) {
        struct sockaddr_in clientAddr{};
        socklen_t          clientLen = sizeof(clientAddr);

        int clientFd = ::accept(listenerFd_,
                                reinterpret_cast<struct sockaddr *>(&clientAddr),
                                &clientLen);
        if (clientFd < 0) {
            if (listenerRunning_.load())
                logger_.log(1, "runListener: accept() — "
                               + std::string(std::strerror(errno)));
            continue;
        }

        char ipBuf[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
        const std::string peerAddr(ipBuf);

        // Check ban list before spending a thread-pool slot
        if (isBanned(peerAddr)) {
            logger_.log(1, "runListener: connection from banned peer '"
                           + peerAddr + "' rejected");
            ::close(clientFd);
            continue;
        }

        setSocketTimeouts(clientFd, cfg_.sendTimeoutMs, cfg_.recvTimeoutMs);

        // Submit to bounded inbound thread pool — no detached threads
        auto fut = inboundPool_.submit(
            [this, clientFd, peerAddr]() {
                handleInbound(clientFd, peerAddr);
            });

        if (!fut.valid()) {
            // Pool is at capacity — apply back-pressure by closing connection
            logger_.log(1, "runListener: inbound pool full — dropping '"
                           + peerAddr + "'");
            ::close(clientFd);
        }
    }
}

void Network::handleInbound(int clientFd, const std::string &peerAddr) noexcept
{
    codec::Frame frame;
    bool ok = recvFrame(clientFd, frame);
    ::close(clientFd);

    if (!ok) {
        logger_.log(1, "handleInbound: recvFrame failed from '" + peerAddr + "'");
        return;
    }

    // Rate limiting and ban enforcement on inbound path
    {
        std::unique_lock<std::shared_mutex> lock(peerMutex_);
        auto it = peers_.find(peerAddr);
        if (it != peers_.end()) {
            if (it->second.isBanned) return;
            if (!checkRateLimit(it->second)) {
                lock.unlock();
                banPeer(peerAddr);
                return;
            }
            it->second.messagesRecv++;
            it->second.lastSeenAt = nowSecs();
        }
    }

    dispatchFrame(frame, peerAddr);
}

void Network::dispatchFrame(const codec::Frame &frame,
                              const std::string  &peerAddr) noexcept
{
    if (frame.type == codec::MessageType::Block) {
        auto blockOpt = codec::decodeBlock(frame.payload);
        if (!blockOpt) {
            logger_.log(2, "dispatchFrame: block decode failed from '" + peerAddr + "'");
            return;
        }
        std::lock_guard<std::mutex> cbLock(callbackMutex_);
        if (onBlockReceivedFn_)
            try { onBlockReceivedFn_(*blockOpt, peerAddr); } catch (...) {}

    } else if (frame.type == codec::MessageType::Transaction) {
        auto txOpt = codec::decodeTransaction(frame.payload);
        if (!txOpt) {
            logger_.log(2, "dispatchFrame: tx decode failed from '" + peerAddr + "'");
            return;
        }
        std::lock_guard<std::mutex> cbLock(callbackMutex_);
        if (onTransactionReceivedFn_)
            try { onTransactionReceivedFn_(*txOpt, peerAddr); } catch (...) {}

    } else if (frame.type == codec::MessageType::Ping) {
        // Respond with Pong using the connection pool
        std::shared_lock<std::shared_mutex> lock(peerMutex_);
        auto it = peers_.find(peerAddr);
        if (it == peers_.end()) return;
        PeerInfo peer = it->second;
        lock.unlock();
        deliverToPeer(peer, codec::MessageType::Pong, {});

    } else if (frame.type == codec::MessageType::Pong) {
        std::unique_lock<std::shared_mutex> lock(peerMutex_);
        auto it = peers_.find(peerAddr);
        if (it != peers_.end()) {
            it->second.lastPongAt  = nowSecs();
            it->second.lastSeenAt  = nowSecs();
            it->second.isReachable = true;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Peer management
// ─────────────────────────────────────────────────────────────────────────────

bool Network::connectToPeer(const std::string &address, uint16_t port) noexcept
{
    if (address.empty()) {
        logger_.log(2, "connectToPeer: empty address rejected");
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> lock(peerMutex_);

        if (peers_.count(address)) {
            logger_.log(1, "connectToPeer: '" + address + "' already connected");
            return false;
        }
        if (peers_.size() >= cfg_.maxPeers) {
            logger_.log(1, "connectToPeer: peer limit reached");
            return false;
        }

        const uint64_t now = nowSecs();
        PeerInfo info;
        info.address     = address;
        info.port        = port;
        info.connectedAt = now;
        info.lastSeenAt  = now;
        info.isReachable = true;
        peers_.emplace(address, info);
    }

    logger_.log(0, "connectToPeer: added '" + address + ":"
                   + std::to_string(port) + "'");

    {
        std::lock_guard<std::mutex> cbLock(callbackMutex_);
        if (onPeerConnectedFn_)
            try { onPeerConnectedFn_(address); } catch (...) {}
    }
    return true;
}

bool Network::disconnectPeer(const std::string &address) noexcept
{
    {
        std::unique_lock<std::shared_mutex> lock(peerMutex_);
        auto it = peers_.find(address);
        if (it == peers_.end()) return false;
        peers_.erase(it);
    }

    connPool_.evictPeer(address, cfg_.listenPort);
    logger_.log(0, "disconnectPeer: removed '" + address + "'");

    {
        std::lock_guard<std::mutex> cbLock(callbackMutex_);
        if (onPeerDisconnectedFn_)
            try { onPeerDisconnectedFn_(address); } catch (...) {}
    }
    return true;
}

bool Network::banPeer(const std::string &address) noexcept
{
    std::unique_lock<std::shared_mutex> lock(peerMutex_);
    auto it = peers_.find(address);
    if (it == peers_.end()) return false;
    it->second.isBanned    = true;
    it->second.banExpiresAt = nowSecs() + cfg_.banDurationSecs;
    lock.unlock();

    connPool_.evictPeer(address, cfg_.listenPort);
    logger_.log(1, "banPeer: '" + address + "' banned for "
                   + std::to_string(cfg_.banDurationSecs) + "s");
    return true;
}

bool Network::isConnected(const std::string &address) const noexcept
{
    std::shared_lock<std::shared_mutex> lock(peerMutex_);
    return peers_.count(address) > 0;
}

bool Network::isBanned(const std::string &address) const noexcept
{
    std::shared_lock<std::shared_mutex> lock(peerMutex_);
    auto it = peers_.find(address);
    if (it == peers_.end()) return false;
    return it->second.isBanned && nowSecs() < it->second.banExpiresAt;
}

std::vector<Network::PeerInfo> Network::getPeers() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(peerMutex_);
    std::vector<PeerInfo> result;
    result.reserve(peers_.size());
    for (const auto &[_, info] : peers_) result.push_back(info);
    return result;
}

size_t Network::peerCount() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(peerMutex_);
    return peers_.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// Broadcasting
// ─────────────────────────────────────────────────────────────────────────────

Network::BroadcastResult
Network::broadcastPayload(codec::MessageType         type,
                           const std::vector<uint8_t> &payload,
                           const std::string          &label) noexcept
{
    BroadcastResult result;

    std::vector<PeerInfo> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(peerMutex_);
        snapshot.reserve(peers_.size());
        for (const auto &[_, info] : peers_)
            if (!info.isBanned) snapshot.push_back(info);
    }

    if (snapshot.empty()) {
        logger_.log(1, label + ": no reachable peers");
        return result;
    }

    for (auto &peer : snapshot) {
        ++result.attempted;
        // Each outbound send is submitted to the bounded outbound pool
        auto fut = outboundPool_.submit([this, &peer, type, &payload, &result]() {
            // Re-acquire a mutable reference from the live map
            std::unique_lock<std::shared_mutex> lock(peerMutex_);
            auto it = peers_.find(peer.address);
            if (it == peers_.end()) { ++result.failed; return; }
            PeerInfo &livePeer = it->second;
            lock.unlock();

            const bool ok = deliverToPeer(livePeer, type, payload);
            if (ok) ++result.succeeded;
            else    ++result.failed;
        });

        if (!fut.valid()) {
            // Outbound pool is saturated — record as failed and continue
            ++result.failed;
            logger_.log(1, label + ": outbound pool full for '"
                           + peer.address + "'");
        }
    }

    // Wait for all in-flight sends to complete before returning the result
    outboundPool_.drain();

    logger_.log(0, label + ": sent=" + std::to_string(result.succeeded)
                   + "/" + std::to_string(result.attempted));
    return result;
}

Network::BroadcastResult
Network::broadcastTransaction(const Transaction &tx) noexcept
{
    if (tx.txHash.empty()) {
        logger_.log(2, "broadcastTransaction: empty txHash — aborted");
        return {};
    }
    return broadcastPayload(codec::MessageType::Transaction,
                             codec::encodeTransaction(tx),
                             "broadcastTx=" + tx.txHash);
}

Network::BroadcastResult
Network::broadcastBlock(const Block &block) noexcept
{
    if (block.hash.empty()) {
        logger_.log(2, "broadcastBlock: empty hash — aborted");
        return {};
    }
    return broadcastPayload(codec::MessageType::Block,
                             codec::encodeBlock(block),
                             "broadcastBlock=" + block.hash);
}

Network::BroadcastResult
Network::broadcastTransactionBatch(
    const std::vector<Transaction> &txs) noexcept
{
    if (txs.empty()) return {};

    // Encode all transactions into a single payload to reduce per-peer
    // round trips and amortise framing overhead
    std::vector<uint8_t> batchPayload;
    batchPayload.reserve(txs.size() * 256);

    const uint32_t count = static_cast<uint32_t>(txs.size());
    batchPayload.push_back((count >> 24) & 0xFF);
    batchPayload.push_back((count >> 16) & 0xFF);
    batchPayload.push_back((count >>  8) & 0xFF);
    batchPayload.push_back( count        & 0xFF);

    for (const auto &tx : txs) {
        auto enc = codec::encodeTransaction(tx);
        const uint32_t len = static_cast<uint32_t>(enc.size());
        batchPayload.push_back((len >> 24) & 0xFF);
        batchPayload.push_back((len >> 16) & 0xFF);
        batchPayload.push_back((len >>  8) & 0xFF);
        batchPayload.push_back( len        & 0xFF);
        batchPayload.insert(batchPayload.end(), enc.begin(), enc.end());
    }

    return broadcastPayload(codec::MessageType::Transaction,
                             batchPayload,
                             "broadcastBatch(" + std::to_string(txs.size()) + " txs)");
}

// ─────────────────────────────────────────────────────────────────────────────
// Health check
// ─────────────────────────────────────────────────────────────────────────────

void Network::runHealthCheck() noexcept
{
    while (healthRunning_.load()) {
        std::this_thread::sleep_for(
            std::chrono::seconds(cfg_.healthCheckSecs));
        if (!healthRunning_.load()) break;
        evictStalePeers();
        unbanExpiredPeers();
    }
}

void Network::evictStalePeers() noexcept
{
    const uint64_t cutoff = nowSecs() - cfg_.peerTimeoutSecs;
    std::vector<std::string> toEvict;
    {
        std::shared_lock<std::shared_mutex> lock(peerMutex_);
        for (const auto &[addr, info] : peers_)
            if (!info.isBanned && info.lastSeenAt < cutoff)
                toEvict.push_back(addr);
    }
    for (const auto &addr : toEvict) {
        logger_.log(1, "evictStalePeers: evicting '" + addr + "'");
        disconnectPeer(addr);
    }
}

void Network::unbanExpiredPeers() noexcept
{
    const uint64_t now = nowSecs();
    std::unique_lock<std::shared_mutex> lock(peerMutex_);
    for (auto &[addr, info] : peers_) {
        if (info.isBanned && now >= info.banExpiresAt) {
            info.isBanned = false;
            logger_.log(0, "unbanExpiredPeers: '" + addr + "' unbanned");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Proactive ping — sends Ping to every peer and evicts those that do not pong
// ─────────────────────────────────────────────────────────────────────────────

void Network::runPingLoop() noexcept
{
    while (pingRunning_.load()) {
        std::this_thread::sleep_for(
            std::chrono::seconds(cfg_.pingIntervalSecs));
        if (!pingRunning_.load()) break;
        sendPings();
    }
}

void Network::sendPings() noexcept
{
    std::vector<PeerInfo> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(peerMutex_);
        for (const auto &[_, info] : peers_)
            if (!info.isBanned) snapshot.push_back(info);
    }

    const uint64_t now = nowSecs();
    for (auto &peer : snapshot) {
        // Evict peers whose last pong is older than the deadline
        if (peer.lastPingSentAt > 0
            && peer.lastPongAt < peer.lastPingSentAt
            && (now - peer.lastPingSentAt) > cfg_.pingDeadlineSecs)
        {
            logger_.log(1, "sendPings: peer '" + peer.address
                           + "' missed pong deadline — evicting");
            disconnectPeer(peer.address);
            continue;
        }

        {
            std::unique_lock<std::shared_mutex> lock(peerMutex_);
            auto it = peers_.find(peer.address);
            if (it != peers_.end())
                it->second.lastPingSentAt = now;
        }

        outboundPool_.submit([this, peer]() mutable {
            deliverToPeer(peer, codec::MessageType::Ping, {});
        });
    }
}
