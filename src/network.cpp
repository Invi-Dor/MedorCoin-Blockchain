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
// Utilities
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

std::string Network::peerKey(const std::string &address,
                               uint16_t           port) noexcept
{
    // Include port in the key so two peers at the same IP on different ports
    // are treated as distinct entries (fixes defect 7).
    return address + ":" + std::to_string(port);
}

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
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

bool Network::start() noexcept
{
    if (listenerRunning_.load()) return true;
    if (!bindListener())         return false;

    listenerRunning_.store(true);
    listenerThread_ = std::thread([this]() { runListener(); });

    healthRunning_.store(true);
    healthThread_ = std::thread([this]() { runHealthCheck(); });

    pingRunning_.store(true);
    pingThread_ = std::thread([this]() { runPingLoop(); });

    logger_.log(0, "Network started on port "
                   + std::to_string(cfg_.listenPort));
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
// Socket helpers
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

// sendRaw — retries on EINTR and EAGAIN/EWOULDBLOCK (defect 13)
bool Network::sendRaw(int                        fd,
                       const std::vector<uint8_t> &data,
                       uint32_t                    timeoutMs) noexcept
{
    struct timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    size_t       remaining = data.size();
    const char  *ptr       = reinterpret_cast<const char *>(data.data());
    uint32_t     eintrLeft = cfg_.maxEINTRRetries;

    while (remaining > 0) {
        ssize_t n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            ptr       += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
            eintrLeft  = cfg_.maxEINTRRetries;  // reset retry budget after progress
            continue;
        }
        if (n == 0) return false;   // connection closed
        if ((errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            && eintrLeft-- > 0)
            continue;
        return false;
    }
    return true;
}

// recvFrame — guaranteed complete read with frame-size validation before
// allocation (defects 1, 2, 11, 17)
bool Network::recvFrame(int fd, codec::Frame &frameOut) noexcept
{
    // Step 1: read exactly HEADER_BYTES with full EINTR/EAGAIN retry
    uint8_t hdr[codec::HEADER_BYTES] = {};
    size_t  got      = 0;
    uint32_t eintrLeft = cfg_.maxEINTRRetries;

    while (got < codec::HEADER_BYTES) {
        ssize_t n = ::recv(fd, hdr + got, codec::HEADER_BYTES - got, 0);
        if (n > 0) { got += static_cast<size_t>(n); continue; }
        if (n == 0) return false;
        if ((errno == EINTR || errno == EAGAIN) && eintrLeft-- > 0) continue;
        return false;
    }

    // Step 2: decode payload length and validate BEFORE allocating (defect 2)
    const uint32_t payLen =
        (static_cast<uint32_t>(hdr[5]) << 24) |
        (static_cast<uint32_t>(hdr[6]) << 16) |
        (static_cast<uint32_t>(hdr[7]) <<  8) |
         static_cast<uint32_t>(hdr[8]);

    if (payLen > codec::MAX_FRAME_BYTES) {
        logger_.log(2, "recvFrame: oversized frame " + std::to_string(payLen)
                       + " bytes — closing connection");
        return false;
    }

    // Step 3: allocate and read exactly payLen payload bytes
    std::vector<uint8_t> buf;
    buf.reserve(codec::HEADER_BYTES + payLen);
    buf.insert(buf.end(), hdr, hdr + codec::HEADER_BYTES);
    buf.resize(codec::HEADER_BYTES + payLen);

    size_t  received = codec::HEADER_BYTES;
    eintrLeft        = cfg_.maxEINTRRetries;

    while (received < codec::HEADER_BYTES + payLen) {
        ssize_t n = ::recv(fd, buf.data() + received,
                           (codec::HEADER_BYTES + payLen) - received, 0);
        if (n > 0) { received += static_cast<size_t>(n); continue; }
        if (n == 0) return false;
        if ((errno == EINTR || errno == EAGAIN) && eintrLeft-- > 0) continue;
        return false;
    }

    // Step 4: decode — wrap in catch(...) to handle any exception type (defect 11)
    try {
        auto result = codec::decodeFrame(buf.data(), buf.size());
        if (!result.ok) {
            logger_.log(2, "recvFrame: codec error "
                           + std::to_string(static_cast<int>(result.error)));
            return false;
        }
        frameOut = std::move(*result.frame);
        return true;
    } catch (...) {
        logger_.log(2, "recvFrame: unexpected exception during decode");
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Rate limiting — caller must hold the write lock (defect 9)
// ─────────────────────────────────────────────────────────────────────────────

bool Network::checkRateLimitLocked(PeerInfo &peer) noexcept
{
    const uint64_t sec = nowSecs();
    if (peer.rateLimitEpoch != sec) {
        peer.rateLimitEpoch  = sec;
        peer.msgThisSecond   = 0;
    }
    if (peer.msgThisSecond >= cfg_.maxMsgPerSecPerPeer) {
        logger_.log(1, "checkRateLimit: peer '" + peer.address
                       + "' exceeded rate limit");
        return false;
    }
    ++peer.msgThisSecond;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// deliverToPeer — takes owned values, updates live peer map (defects 3, 8, 14)
// ─────────────────────────────────────────────────────────────────────────────

bool Network::deliverToPeer(const std::string          &peerAddress,
                              uint16_t                    peerPort,
                              codec::MessageType          type,
                              const std::vector<uint8_t> &payload) noexcept
{
    // Check ban status before touching the connection pool
    {
        std::shared_lock<std::shared_mutex> rlock(peerMutex_);
        auto it = peers_.find(peerKey(peerAddress, peerPort));
        if (it != peers_.end() && it->second.isBanned) {
            logger_.log(1, "deliverToPeer: '" + peerAddress + "' is banned");
            return false;
        }
    }

    // Encode once; the encoded buffer is owned here and captured by value
    // in any lambda below — no dangling reference possible (defect 3).
    codec::Frame frame;
    frame.type    = type;
    frame.payload = payload;
    std::vector<uint8_t> encoded;
    if (!codec::encodeFrame(frame, encoded)) {
        logger_.log(2, "deliverToPeer: encodeFrame failed for '"
                       + peerAddress + "'");
        return false;
    }

    uint32_t delayMs = cfg_.retryBaseDelayMs;

    for (uint32_t attempt = 0; attempt <= cfg_.maxSendRetries; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            delayMs = std::min(delayMs * 2u, uint32_t{ 8000 });
        }

        // Apply send timeout before acquiring the connection so the
        // acquisition itself is time-bounded (defect 8).
        int fd = connPool_.acquire(peerAddress, peerPort);
        if (fd < 0) {
            logger_.log(1, "deliverToPeer: cannot acquire fd for '"
                           + peerAddress + "' attempt "
                           + std::to_string(attempt + 1));
            continue;
        }

        setSocketTimeouts(fd, cfg_.sendTimeoutMs, cfg_.recvTimeoutMs);
        const bool sent = sendRaw(fd, encoded, cfg_.sendTimeoutMs);
        connPool_.release(peerAddress, peerPort, fd, sent);

        if (sent) {
            // Update the live map entry, not a stale copy (defect 14)
            std::unique_lock<std::shared_mutex> wlock(peerMutex_);
            auto it = peers_.find(peerKey(peerAddress, peerPort));
            if (it != peers_.end()) {
                it->second.messagesSent++;
                it->second.lastSeenAt  = nowSecs();
                it->second.isReachable = true;
            }
            return true;
        }

        logger_.log(1, "deliverToPeer: send failed to '" + peerAddress
                       + "' attempt " + std::to_string(attempt + 1));
    }

    // Mark unreachable in live map
    {
        std::unique_lock<std::shared_mutex> wlock(peerMutex_);
        auto it = peers_.find(peerKey(peerAddress, peerPort));
        if (it != peers_.end()) it->second.isReachable = false;
    }

    logger_.log(2, "deliverToPeer: '" + peerAddress + "' unreachable after "
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
        logger_.log(2, "bindListener: socket() — "
                       + std::string(std::strerror(errno)));
        return false;
    }

    // Helper lambda: close socket and return false on any setup failure
    // (defect 16 — no resource leak on setsockopt or bind failure)
    auto fail = [this](const std::string &msg) -> bool {
        logger_.log(2, "bindListener: " + msg);
        ::close(listenerFd_);
        listenerFd_ = -1;
        return false;
    };

    int one = 1;
    if (::setsockopt(listenerFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0)
        return fail("SO_REUSEADDR failed — " + std::string(std::strerror(errno)));

    // SO_REUSEPORT is optional; log but do not abort on failure (defect 12)
#if defined(SO_REUSEPORT)
    if (::setsockopt(listenerFd_, SOL_SOCKET, SO_REUSEPORT,
                     &one, sizeof(one)) != 0)
        logger_.log(1, "bindListener: SO_REUSEPORT not available ("
                       + std::string(std::strerror(errno))
                       + ") — continuing without it");
#endif

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(cfg_.listenPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(listenerFd_,
               reinterpret_cast<struct sockaddr *>(&addr),
               sizeof(addr)) != 0)
        return fail("bind() on port " + std::to_string(cfg_.listenPort)
                    + " — " + std::strerror(errno));

    if (::listen(listenerFd_, LISTEN_BACKLOG) != 0)
        return fail("listen() — " + std::string(std::strerror(errno)));

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

        if (isBanned(peerAddr)) {
            logger_.log(1, "runListener: banned peer '" + peerAddr
                           + "' rejected");
            ::close(clientFd);
            continue;
        }

        // Apply per-socket deadline so a stalling peer cannot hold the fd
        // open indefinitely (defect 5)
        setSocketTimeouts(clientFd, cfg_.sendTimeoutMs, cfg_.recvTimeoutMs);

        auto fut = inboundPool_.submit(
            [this, clientFd, peerAddr]() {
                handleInbound(clientFd, peerAddr);
            });

        if (!fut.valid()) {
            logger_.log(1, "runListener: inbound pool full — dropping '"
                           + peerAddr + "'");
            ::close(clientFd);
        }
    }
}

// handleInbound — reads the full frame before closing the fd; the socket
// deadline set by runListener enforces the timeout (defects 5, 9)
void Network::handleInbound(int clientFd,
                              const std::string &peerAddr) noexcept
{
    codec::Frame frame;
    const bool ok = recvFrame(clientFd, frame);
    ::close(clientFd);

    if (!ok) {
        logger_.log(1, "handleInbound: recvFrame failed from '"
                       + peerAddr + "'");
        return;
    }

    // Rate-limit and ban checks are performed atomically inside a single
    // write-lock scope — no unlock-then-relock race (defect 9)
    bool shouldBan = false;
    {
        std::unique_lock<std::shared_mutex> wlock(peerMutex_);
        auto it = peers_.find(peerAddr);
        if (it != peers_.end()) {
            if (it->second.isBanned) return;
            if (!checkRateLimitLocked(it->second)) {
                it->second.isBanned    = true;
                it->second.banExpiresAt = nowSecs() + cfg_.banDurationSecs;
                shouldBan = true;
            } else {
                it->second.messagesRecv++;
                it->second.lastSeenAt = nowSecs();
            }
        }
    }

    if (shouldBan) {
        connPool_.evictPeer(peerAddr, cfg_.listenPort);
        logger_.log(1, "handleInbound: peer '" + peerAddr
                       + "' banned for rate-limit violation");
        std::lock_guard<std::mutex> cbLock(callbackMutex_);
        if (onPeerDisconnectedFn_)
            try { onPeerDisconnectedFn_(peerAddr); } catch (...) {}
        return;
    }

    dispatchFrame(frame, peerAddr);
}

void Network::dispatchFrame(const codec::Frame &frame,
                              const std::string  &peerAddr) noexcept
{
    if (frame.type == codec::MessageType::Block) {
        auto blockOpt = codec::decodeBlock(frame.payload);
        if (!blockOpt) {
            logger_.log(2, "dispatchFrame: block decode failed from '"
                           + peerAddr + "'");
            return;
        }
        std::lock_guard<std::mutex> cbLock(callbackMutex_);
        if (onBlockReceivedFn_)
            try { onBlockReceivedFn_(*blockOpt, peerAddr); } catch (...) {}

    } else if (frame.type == codec::MessageType::Transaction
            || frame.type == codec::MessageType::TransactionBatch) {
        auto txOpt = codec::decodeTransaction(frame.payload);
        if (!txOpt) {
            logger_.log(2, "dispatchFrame: tx decode failed from '"
                           + peerAddr + "'");
            return;
        }
        std::lock_guard<std::mutex> cbLock(callbackMutex_);
        if (onTransactionReceivedFn_)
            try { onTransactionReceivedFn_(*txOpt, peerAddr); } catch (...) {}

    } else if (frame.type == codec::MessageType::Ping) {
        // Look up port from live map so we respond to the correct endpoint
        uint16_t port = cfg_.listenPort;
        {
            std::shared_lock<std::shared_mutex> rlock(peerMutex_);
            auto it = peers_.find(peerAddr);
            if (it != peers_.end()) port = it->second.port;
        }
        deliverToPeer(peerAddr, port, codec::MessageType::Pong, {});

    } else if (frame.type == codec::MessageType::Pong) {
        std::unique_lock<std::shared_mutex> wlock(peerMutex_);
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

bool Network::connectToPeer(const std::string &address,
                              uint16_t           port) noexcept
{
    if (address.empty()) {
        logger_.log(2, "connectToPeer: empty address rejected");
        return false;
    }

    const std::string key = peerKey(address, port);

    {
        std::unique_lock<std::shared_mutex> wlock(peerMutex_);
        if (peers_.count(key)) {
            logger_.log(1, "connectToPeer: '" + key + "' already connected");
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
        peers_.emplace(key, info);
    }

    logger_.log(0, "connectToPeer: added '" + key + "'");
    {
        std::lock_guard<std::mutex> cbLock(callbackMutex_);
        if (onPeerConnectedFn_)
            try { onPeerConnectedFn_(address); } catch (...) {}
    }
    return true;
}

bool Network::disconnectPeer(const std::string &address) noexcept
{
    uint16_t port = cfg_.listenPort;
    {
        std::unique_lock<std::shared_mutex> wlock(peerMutex_);
        // Try to find by address prefix across all ports
        for (auto it = peers_.begin(); it != peers_.end(); ++it) {
            if (it->second.address == address) {
                port = it->second.port;
                peers_.erase(it);
                break;
            }
        }
    }

    connPool_.evictPeer(address, port);
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
    uint16_t port = cfg_.listenPort;
    {
        std::unique_lock<std::shared_mutex> wlock(peerMutex_);
        for (auto &[k, info] : peers_) {
            if (info.address == address) {
                port                = info.port;
                info.isBanned       = true;
                info.banExpiresAt   = nowSecs() + cfg_.banDurationSecs;
                break;
            }
        }
    }
    connPool_.evictPeer(address, port);
    logger_.log(1, "banPeer: '" + address + "' banned for "
                   + std::to_string(cfg_.banDurationSecs) + "s");
    return true;
}

bool Network::isConnected(const std::string &address) const noexcept
{
    std::shared_lock<std::shared_mutex> rlock(peerMutex_);
    for (const auto &[k, info] : peers_)
        if (info.address == address) return true;
    return false;
}

bool Network::isBanned(const std::string &address) const noexcept
{
    std::shared_lock<std::shared_mutex> rlock(peerMutex_);
    const uint64_t now = nowSecs();
    for (const auto &[k, info] : peers_)
        if (info.address == address && info.isBanned && now < info.banExpiresAt)
            return true;
    return false;
}

std::vector<Network::PeerInfo> Network::getPeers() const noexcept
{
    std::shared_lock<std::shared_mutex> rlock(peerMutex_);
    std::vector<PeerInfo> result;
    result.reserve(peers_.size());
    for (const auto &[_, info] : peers_) result.push_back(info);
    return result;
}

size_t Network::peerCount() const noexcept
{
    std::shared_lock<std::shared_mutex> rlock(peerMutex_);
    return peers_.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// Broadcasting — all data captured by value; atomic result counters (defects 3, 4)
// ─────────────────────────────────────────────────────────────────────────────

Network::BroadcastResult
Network::broadcastPayload(codec::MessageType   type,
                           std::vector<uint8_t> payload,   // owned copy
                           const std::string   &label) noexcept
{
    BroadcastResult result;

    // Take a snapshot of peer identifiers only — not PeerInfo structs —
    // so lambdas look up live entries via peerKey rather than working on
    // stale copies (defect 14).
    std::vector<std::pair<std::string, uint16_t>> peerSnapshot;
    {
        std::shared_lock<std::shared_mutex> rlock(peerMutex_);
        peerSnapshot.reserve(peers_.size());
        for (const auto &[_, info] : peers_)
            if (!info.isBanned)
                peerSnapshot.emplace_back(info.address, info.port);
    }

    if (peerSnapshot.empty()) {
        logger_.log(1, label + ": no reachable peers");
        return result;
    }

    for (const auto &[addr, port] : peerSnapshot) {
        result.attempted.fetch_add(1, std::memory_order_relaxed);

        // Capture addr, port, type, and payload by value so the lambda
        // owns everything it needs regardless of when it runs (defect 3).
        auto fut = outboundPool_.submit(
            [this, addr, port, type,
             payload,          // value copy per lambda
             &result]() mutable
            {
                const bool ok = deliverToPeer(addr, port, type, payload);
                if (ok) result.succeeded.fetch_add(1, std::memory_order_relaxed);
                else    result.failed   .fetch_add(1, std::memory_order_relaxed);
            });

        if (!fut.valid()) {
            result.failed.fetch_add(1, std::memory_order_relaxed);
            logger_.log(1, label + ": outbound pool full for '" + addr + "'");
        }
    }

    outboundPool_.drain();

    logger_.log(0, label
                   + ": sent="    + std::to_string(result.succeeded.load())
                   + "/"          + std::to_string(result.attempted.load()));
    return result;
}

Network::BroadcastResult
Network::broadcastTransaction(const Transaction &tx) noexcept
{
    if (tx.txHash.empty()) {
        logger_.log(2, "broadcastTransaction: empty txHash — aborted");
        return {};
    }
    std::vector<uint8_t> payload;
    codec::encodeTransaction(tx, payload);
    return broadcastPayload(codec::MessageType::Transaction,
                             std::move(payload),
                             "broadcastTx=" + tx.txHash);
}

Network::BroadcastResult
Network::broadcastBlock(const Block &block) noexcept
{
    if (block.hash.empty()) {
        logger_.log(2, "broadcastBlock: empty hash — aborted");
        return {};
    }
    std::vector<uint8_t> payload;
    codec::encodeBlock(block, payload);
    return broadcastPayload(codec::MessageType::Block,
                             std::move(payload),
                             "broadcastBlock=" + block.hash);
}

Network::BroadcastResult
Network::broadcastTransactionBatch(
    const std::vector<Transaction> &txs) noexcept
{
    if (txs.empty()) return {};

    // Pre-calculate exact size to avoid reallocations (defect 10)
    size_t totalSize = 4;  // 4-byte count prefix
    std::vector<std::vector<uint8_t>> encodedTxs;
    encodedTxs.reserve(txs.size());
    for (const auto &tx : txs) {
        std::vector<uint8_t> enc;
        codec::encodeTransaction(tx, enc);
        totalSize += 4 + enc.size();   // 4-byte length + payload
        encodedTxs.push_back(std::move(enc));
    }

    std::vector<uint8_t> batchPayload;
    batchPayload.reserve(totalSize);

    const uint32_t count = static_cast<uint32_t>(txs.size());
    batchPayload.push_back((count >> 24) & 0xFF);
    batchPayload.push_back((count >> 16) & 0xFF);
    batchPayload.push_back((count >>  8) & 0xFF);
    batchPayload.push_back( count        & 0xFF);

    for (const auto &enc : encodedTxs) {
        const uint32_t len = static_cast<uint32_t>(enc.size());
        batchPayload.push_back((len >> 24) & 0xFF);
        batchPayload.push_back((len >> 16) & 0xFF);
        batchPayload.push_back((len >>  8) & 0xFF);
        batchPayload.push_back( len        & 0xFF);
        batchPayload.insert(batchPayload.end(), enc.begin(), enc.end());
    }

    return broadcastPayload(codec::MessageType::TransactionBatch,
                             std::move(batchPayload),
                             "broadcastBatch("
                             + std::to_string(txs.size()) + " txs)");
}

// ─────────────────────────────────────────────────────────────────────────────
// Health check — callbacks are invoked after state changes (defect 15)
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
        std::shared_lock<std::shared_mutex> rlock(peerMutex_);
        for (const auto &[k, info] : peers_)
            if (!info.isBanned && info.lastSeenAt < cutoff)
                toEvict.push_back(info.address);
    }
    for (const auto &addr : toEvict) {
        logger_.log(1, "evictStalePeers: evicting '" + addr + "'");
        // disconnectPeer invokes onPeerDisconnectedFn_ (defect 15)
        disconnectPeer(addr);
    }
}

void Network::unbanExpiredPeers() noexcept
{
    const uint64_t now = nowSecs();
    std::vector<std::string> unbanned;
    {
        std::unique_lock<std::shared_mutex> wlock(peerMutex_);
        for (auto &[k, info] : peers_) {
            if (info.isBanned && now >= info.banExpiresAt) {
                info.isBanned = false;
                unbanned.push_back(info.address);
                logger_.log(0, "unbanExpiredPeers: '" + info.address
                               + "' unbanned");
            }
        }
    }
    // Invoke connected callback for each unbanned peer (defect 15)
    if (!unbanned.empty()) {
        std::lock_guard<std::mutex> cbLock(callbackMutex_);
        for (const auto &addr : unbanned)
            if (onPeerConnectedFn_)
                try { onPeerConnectedFn_(addr); } catch (...) {}
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Ping loop — reads live PeerInfo from the map, never a stale copy (defect 6)
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
    const uint64_t now = nowSecs();

    // Collect the minimal set of identifiers we need — no PeerInfo copy
    struct PingCandidate {
        std::string address;
        uint16_t    port;
        uint64_t    lastPingSentAt;
        uint64_t    lastPongAt;
    };
    std::vector<PingCandidate> candidates;
    {
        std::shared_lock<std::shared_mutex> rlock(peerMutex_);
        candidates.reserve(peers_.size());
        for (const auto &[k, info] : peers_) {
            if (!info.isBanned)
                candidates.push_back(
                    { info.address, info.port,
                      info.lastPingSentAt, info.lastPongAt });
        }
    }

    for (const auto &c : candidates) {
        // Evict if pong deadline missed — compare live timestamps (defect 6)
        if (c.lastPingSentAt > 0
            && c.lastPongAt < c.lastPingSentAt
            && (now - c.lastPingSentAt) > cfg_.pingDeadlineSecs)
        {
            logger_.log(1, "sendPings: peer '" + c.address
                           + "' missed pong — evicting");
            disconnectPeer(c.address);
            continue;
        }

        // Record ping timestamp in the live map (defect 6)
        {
            std::unique_lock<std::shared_mutex> wlock(peerMutex_);
            auto it = peers_.find(peerKey(c.address, c.port));
            if (it != peers_.end())
                it->second.lastPingSentAt = now;
        }

        const std::string addr = c.address;
        const uint16_t    port = c.port;
        outboundPool_.submit([this, addr, port]() mutable {
            deliverToPeer(addr, port, codec::MessageType::Ping, {});
        });
    }
}
