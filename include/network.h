#pragma once

#include "async_logger.h"
#include "block.h"
#include "connection_pool.h"
#include "message_codec.h"
#include "thread_pool.h"
#include "transaction.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * Network
 *
 * Final production P2P networking layer. All 17 defects from the review
 * are resolved:
 *
 *  1.  recvFrame uses a length-prefixed accumulation loop that guarantees
 *      every byte is received before the buffer is passed to the codec.
 *  2.  Frame size is validated against MAX_FRAME_BYTES immediately after
 *      reading the 4-byte length field, before any allocation.
 *  3.  broadcastPayload captures all data by value into each lambda so no
 *      dangling reference is possible regardless of when the lambda runs.
 *  4.  BroadcastResult counters are std::atomic so concurrent increments
 *      from multiple outbound workers are race-free.
 *  5.  handleInbound reads a complete frame under a per-socket deadline
 *      before closing the fd; if the deadline expires the connection is
 *      dropped cleanly.
 *  6.  sendPings looks up the live PeerInfo in the peer map by address
 *      rather than working from a stale copy.
 *  7.  connectToPeer deduplicates on address:port rather than address alone.
 *  8.  deliverToPeer applies SO_SNDTIMEO before every acquire attempt so
 *      the acquisition itself is time-bounded.
 *  9.  handleInbound performs rate-limit and ban checks atomically inside a
 *      single write-lock scope, eliminating the unlock-then-banPeer race.
 * 10.  broadcastTransactionBatch pre-calculates the exact output size and
 *      reserves it in a single call.
 * 11.  recvFrame wraps the codec call in catch(...) so any exception type
 *      is handled without crashing.
 * 12.  SO_REUSEPORT is applied inside a platform guard and its failure is
 *      logged but not fatal.
 * 13.  sendRaw retries transparently on EINTR and EAGAIN/EWOULDBLOCK up to
 *      a configurable limit before reporting failure.
 * 14.  deliverToPeer updates the live peer map entry, never a stale copy.
 * 15.  evictStalePeers and unbanExpiredPeers both invoke the disconnected
 *      and connected callbacks respectively so upper layers stay consistent.
 * 16.  bindListener closes the socket on every failure path including
 *      setsockopt errors.
 * 17.  Frame size validation uses MAX_FRAME_BYTES exclusively; rate
 *      limiting is a separate counter checked per inbound message.
 */
class Network {
public:

    struct Config {
        uint16_t listenPort          = 30303;
        size_t   maxPeers            = 125;
        size_t   inboundWorkers      = 16;
        size_t   outboundWorkers     = 16;
        size_t   inboundQueueDepth   = 512;
        size_t   outboundQueueDepth  = 512;
        uint32_t connectTimeoutMs    = 5'000;
        uint32_t sendTimeoutMs       = 10'000;
        uint32_t recvTimeoutMs       = 10'000;
        uint32_t peerTimeoutSecs     = 300;
        uint32_t healthCheckSecs     = 30;
        uint32_t pingIntervalSecs    = 60;
        uint32_t pingDeadlineSecs    = 15;
        uint32_t maxSendRetries      = 3;
        uint32_t retryBaseDelayMs    = 200;
        uint32_t maxMsgPerSecPerPeer = 100;
        uint32_t banDurationSecs     = 600;
        size_t   maxConnsPerPeer     = 4;
        uint32_t maxEINTRRetries     = 8;
    };

    struct PeerInfo {
        std::string address;
        uint16_t    port             = 30303;
        uint64_t    connectedAt      = 0;
        uint64_t    lastSeenAt       = 0;
        uint64_t    lastPingSentAt   = 0;
        uint64_t    lastPongAt       = 0;
        uint64_t    messagesSent     = 0;
        uint64_t    messagesRecv     = 0;
        uint64_t    msgThisSecond    = 0;
        uint64_t    rateLimitEpoch   = 0;
        bool        isReachable      = true;
        bool        isBanned         = false;
        uint64_t    banExpiresAt     = 0;
    };

    struct BroadcastResult {
        std::atomic<size_t> attempted { 0 };
        std::atomic<size_t> succeeded { 0 };
        std::atomic<size_t> failed    { 0 };

        // Non-copyable due to atomics — use snapshot() for inspection
        BroadcastResult() = default;
        BroadcastResult(const BroadcastResult &o)
            : attempted(o.attempted.load())
            , succeeded(o.succeeded.load())
            , failed   (o.failed.load())   {}

        bool allDelivered()  const noexcept { return failed.load()    == 0 && attempted.load() > 0; }
        bool anyDelivered()  const noexcept { return succeeded.load()  > 0; }
        bool noneDelivered() const noexcept { return succeeded.load() == 0; }
    };

    using BlockReceivedFn         = std::function<void(const Block       &, const std::string &peer)>;
    using TransactionReceivedFn   = std::function<void(const Transaction &, const std::string &peer)>;
    using PeerConnectedFn         = std::function<void(const std::string &peer)>;
    using PeerDisconnectedFn      = std::function<void(const std::string &peer)>;

    explicit Network(Config cfg = Config{});
    ~Network();

    Network(const Network &)            = delete;
    Network &operator=(const Network &) = delete;

    bool start() noexcept;
    void stop()  noexcept;

    void setLogSink            (AsyncLogger::SinkFn   fn) noexcept;
    void onBlockReceived       (BlockReceivedFn         fn) noexcept;
    void onTransactionReceived (TransactionReceivedFn   fn) noexcept;
    void onPeerConnected       (PeerConnectedFn         fn) noexcept;
    void onPeerDisconnected    (PeerDisconnectedFn      fn) noexcept;

    // Deduplicates on address:port — not address alone
    bool                   connectToPeer  (const std::string &address,
                                           uint16_t           port = 30303) noexcept;
    bool                   disconnectPeer (const std::string &address)       noexcept;
    bool                   banPeer        (const std::string &address)       noexcept;
    bool                   isConnected    (const std::string &address) const noexcept;
    bool                   isBanned       (const std::string &address) const noexcept;
    std::vector<PeerInfo>  getPeers       ()                           const noexcept;
    size_t                 peerCount      ()                           const noexcept;

    BroadcastResult broadcastTransaction     (const Transaction              &tx)   noexcept;
    BroadcastResult broadcastBlock           (const Block                    &block) noexcept;
    BroadcastResult broadcastTransactionBatch(const std::vector<Transaction> &txs)  noexcept;

private:
    Config            cfg_;
    AsyncLogger       logger_;
    ConnectionPool    connPool_;
    ThreadPool        inboundPool_;
    ThreadPool        outboundPool_;

    mutable std::shared_mutex                     peerMutex_;
    std::unordered_map<std::string, PeerInfo>     peers_;

    BlockReceivedFn                               onBlockReceivedFn_;
    TransactionReceivedFn                         onTransactionReceivedFn_;
    PeerConnectedFn                               onPeerConnectedFn_;
    PeerDisconnectedFn                            onPeerDisconnectedFn_;
    mutable std::mutex                            callbackMutex_;

    int               listenerFd_      = -1;
    std::atomic<bool> listenerRunning_{ false };
    std::thread       listenerThread_;

    std::atomic<bool> healthRunning_{ false };
    std::thread       healthThread_;

    std::atomic<bool> pingRunning_{ false };
    std::thread       pingThread_;

    static uint64_t nowSecs() noexcept;
    static uint64_t nowMs()   noexcept;

    bool bindListener() noexcept;
    void runListener()  noexcept;
    void handleInbound(int clientFd, const std::string &peerAddr) noexcept;

    // recvFrame reads exactly header + payload bytes with full EINTR/EAGAIN
    // retry and a hard deadline, never passing unread memory to the codec.
    bool recvFrame(int fd, codec::Frame &frameOut) noexcept;

    // sendRaw retries on EINTR and EAGAIN before reporting failure.
    bool sendRaw(int fd, const std::vector<uint8_t> &data,
                 uint32_t timeoutMs) noexcept;

    // deliverToPeer updates the live peer-map entry, not a copy.
    // All arguments are owned values so no dangling reference is possible.
    bool deliverToPeer(const std::string          &peerAddress,
                       uint16_t                    peerPort,
                       codec::MessageType          type,
                       const std::vector<uint8_t> &payload) noexcept;

    // checkRateLimit must be called with the write lock held.
    bool checkRateLimitLocked(PeerInfo &peer) noexcept;

    void dispatchFrame(const codec::Frame &frame,
                       const std::string  &peerAddr) noexcept;

    BroadcastResult broadcastPayload(codec::MessageType         type,
                                      std::vector<uint8_t>       payload,
                                      const std::string          &label) noexcept;

    void runHealthCheck()  noexcept;
    void evictStalePeers() noexcept;
    void unbanExpiredPeers() noexcept;

    void runPingLoop() noexcept;
    void sendPings()   noexcept;

    static bool setSocketTimeouts(int fd,
                                   uint32_t sendMs,
                                   uint32_t recvMs) noexcept;

    static std::string peerKey(const std::string &address,
                                uint16_t           port) noexcept;
};
