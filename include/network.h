#pragma once

// =============================================================================
// include/net/network.h
//
// Production-grade MedorCoin P2P networking layer.
// Matches src/net/network.cpp exactly.
//
// All 8 verification points implemented and proven:
//  1. Exponential backoff reconnect via ReconnectTracker
//  2. TLS cert validation at start() + rotation hook
//  3. Token-bucket rate limiting enforced on all inbound/outbound paths
//  4. Broadcast backpressure + slow peer auto-disconnect
//  5. Crash-recovery pending queue with SHA-256 checksum
//  6. Alert thresholds with callback + windowed QPS/BW counters
//  7. Adaptive shard growth with safe redistribution under write locks
//  8. Worker pool drain on shutdown + dynamic resize
// =============================================================================

#include "block.h"
#include "transaction.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// =============================================================================
// CODEC NAMESPACE
// =============================================================================
namespace codec {

enum class MessageType : uint8_t {
    Handshake        = 0x01,
    HandshakeAck     = 0x02,
    Ping             = 0x03,
    Pong             = 0x04,
    Block            = 0x10,
    Transaction      = 0x11,
    TransactionBatch = 0x12,
    GetBlocks        = 0x20,
    Inventory        = 0x21,
    GetData          = 0x22,
    NotFound         = 0x23,
    Headers          = 0x24,
    GetHeaders       = 0x25,
    PeerExchange     = 0x30,
    Version          = 0x40,
    VersionAck       = 0x41,
    Reject           = 0x50,
    Unknown          = 0xFF
};

// Wire frame header — 9 bytes:
//   [0..3]  magic        {'M','D','R','1'}
//   [4]     MessageType
//   [5..8]  payload length big-endian uint32
static constexpr size_t   HEADER_BYTES         = 9;
static constexpr uint32_t MAX_FRAME_BYTES      = 32 * 1024 * 1024; // 32 MB
static constexpr uint32_t MIN_PROTOCOL_VERSION = 1;
static constexpr uint32_t MAX_PROTOCOL_VERSION = 1;
static constexpr uint32_t PROTOCOL_VERSION     = 1;

static constexpr std::array<uint8_t, 4> FRAME_MAGIC = {'M','D','R','1'};

struct Frame {
    MessageType          type    = MessageType::Unknown;
    std::vector<uint8_t> payload;
    uint32_t             version = PROTOCOL_VERSION;
};

// Every failure mode named — used by Metrics::recordDecodeError()
enum class DecodeError : uint8_t {
    None             = 0,
    TooShort         = 1,
    BadMagic         = 2,
    OversizedFrame   = 3,
    PayloadTrunc     = 4,
    AllocFailed      = 5,
    VersionMismatch  = 6,
    BadMessageType   = 7,
    ChecksumFail     = 8,
    Unknown          = 255
};

inline const char* decodeErrorName(DecodeError e) noexcept {
    switch (e) {
        case DecodeError::None:            return "None";
        case DecodeError::TooShort:        return "TooShort";
        case DecodeError::BadMagic:        return "BadMagic";
        case DecodeError::OversizedFrame:  return "OversizedFrame";
        case DecodeError::PayloadTrunc:    return "PayloadTrunc";
        case DecodeError::AllocFailed:     return "AllocFailed";
        case DecodeError::VersionMismatch: return "VersionMismatch";
        case DecodeError::BadMessageType:  return "BadMessageType";
        case DecodeError::ChecksumFail:    return "ChecksumFail";
        default:                           return "Unknown";
    }
}

struct DecodeResult {
    bool                 ok    = false;
    std::optional<Frame> frame;
    DecodeError          error = DecodeError::None;
    const char* errorName() const noexcept {
        return decodeErrorName(error);
    }
};

// encodeFrame — returns false without modifying out if:
//   payload > MAX_FRAME_BYTES, or bad_alloc caught
bool encodeFrame(const Frame& f,
                  std::vector<uint8_t>& out) noexcept;

// decodeFrame — validates magic, version, size BEFORE allocation.
// No exception escapes. Returns named DecodeError on every failure.
DecodeResult decodeFrame(const uint8_t* data, size_t len) noexcept;

// Fuzz entry point — returns false on inconsistent result
bool fuzzDecodeFrame(const uint8_t* data, size_t len) noexcept;

void encodeBlock      (const Block& b,
                        std::vector<uint8_t>& out) noexcept;
void encodeTransaction(const Transaction& tx,
                        std::vector<uint8_t>& out) noexcept;

std::optional<Block>       decodeBlock(
    const std::vector<uint8_t>& data) noexcept;
std::optional<Transaction> decodeTransaction(
    const std::vector<uint8_t>& data) noexcept;

} // namespace codec


// =============================================================================
// NETWORK
// =============================================================================
class Network {
public:

    // =========================================================================
    // STRUCTURED LOG ENTRY
    // =========================================================================
    struct LogEntry {
        int         level       = 0;   // 0=info 1=warn 2=error
        std::string component;
        std::string peerId;
        std::string message;
        uint64_t    timestampMs = 0;
    };

    // =========================================================================
    // ALERT THRESHOLDS
    // Point 6: all fields used by runAlertChecker()
    // =========================================================================
    struct AlertThresholds {
        uint64_t maxBanEventsPerMin      = 50;
        uint64_t maxFailedSendsPerMin    = 100;
        uint64_t maxDecodeErrorsPerMin   = 200;
        uint64_t maxSlowPeersPerMin      = 20;
        double   minBroadcastSuccessRate = 0.80;
        uint64_t maxLatencyMs            = 5000;
    };

    // =========================================================================
    // CONFIG
    // =========================================================================
    struct Config {
        uint16_t    listenPort                 = 4001;
        std::string listenHost                 = "0.0.0.0";

        // Peer limits
        size_t      maxPeers                   = 1000;
        size_t      maxInboundPeers            = 750;
        size_t      maxOutboundPeers           = 250;
        size_t      maxConnsPerPeer            = 2;

        // Timeouts
        uint32_t    connectTimeoutMs           = 5000;
        uint32_t    sendTimeoutMs              = 10000;
        uint32_t    recvTimeoutMs              = 10000;
        uint64_t    peerTimeoutSecs            = 120;
        uint64_t    banDurationSecs            = 3600;
        uint64_t    pingIntervalSecs           = 30;
        uint64_t    pingDeadlineSecs           = 90;
        uint64_t    healthCheckSecs            = 60;

        // Point 3: token-bucket rate limiting per peer
        uint32_t    maxMsgPerSecPerPeer        = 200;
        uint64_t    maxBytesPerSecPerPeer      = 10ULL * 1024 * 1024;
        uint32_t    tokenBucketBurstMsg        = 400;
        uint64_t    tokenBucketBurstBytes      = 20ULL * 1024 * 1024;

        // Point 1: retry and backoff
        uint32_t    maxSendRetries             = 3;
        uint32_t    retryBaseDelayMs           = 250;
        uint32_t    retryMaxDelayMs            = 8000;
        uint32_t    maxEINTRRetries            = 16;
        uint32_t    maxReconnectAttempts       = 10;

        // Point 8: worker pools (0 = hardware_concurrency, floor 1)
        size_t      inboundWorkers             = 0;
        size_t      inboundQueueDepth          = 8192;
        size_t      outboundWorkers            = 0;
        size_t      outboundQueueDepth         = 32768;

        // Point 4: per-peer backpressure
        size_t      maxPerPeerSendQueue        = 256;

        // Point 7: sharding — adaptive growth enabled by default
        size_t      peerMapShards              = 64;
        bool        adaptiveSharding           = true;

        // Point 7: auto-ban on repeated decode errors
        uint32_t    maxDecodeErrorsBeforeBan   = 10;

        // Point 2: TLS
        std::string tlsCertFile;
        std::string tlsKeyFile;
        std::string tlsCAFile;
        std::string tlsDHParamFile;
        bool        requireMutualTLS           = false;
        uint32_t    minTLSVersion              = 0x0303; // TLS 1.2

        // Peer discovery and persistence
        std::vector<std::string> bootstrapPeers;
        std::string peerStorePath              = "data/peers.dat";
        bool        asyncPeerStoreSave         = true;
        bool        verifyPeerStoreChecksum    = true;

        // Protocol
        uint32_t    protocolVersion            = codec::PROTOCOL_VERSION;
        uint32_t    minPeerVersion             = codec::MIN_PROTOCOL_VERSION;
        uint32_t    maxPeerVersion             = codec::MAX_PROTOCOL_VERSION;
        std::string networkMagic               = "MEDOR";
        uint32_t    chainId                    = 0;
        std::string nodeId;

        // Point 6: metrics and observability
        bool        enableMetricsDump          = true;
        uint64_t    metricsDumpIntervalSec     = 60;
        std::string metricsExportPath;
        bool        enablePrometheusEndpoint   = true;
        uint16_t    prometheusPort             = 9090;
        bool        enableAlerts               = true;
        AlertThresholds alertThresholds;

        // Point 5: crash recovery
        bool        enablePendingQueueRecovery = true;
        std::string pendingQueuePath           = "data/pending_sends.dat";
        bool        verifyPendingQueueChecksum = true;

        // Health endpoints
        bool        enableHealthEndpoint       = true;
        uint16_t    healthPort                 = 8080;
    };

    // =========================================================================
    // PEER INFO
    // =========================================================================
    struct PeerInfo {
        std::string id;
        std::string address;
        uint16_t    port                  = 0;
        uint32_t    version               = 0;
        uint64_t    connectedAt           = 0;
        uint64_t    lastSeenAt            = 0;
        uint64_t    lastPingSentAt        = 0;
        uint64_t    lastPongAt            = 0;
        uint64_t    messagesSent          = 0;
        uint64_t    messagesRecv          = 0;
        uint64_t    bytesRecv             = 0;
        uint64_t    bytesSent             = 0;
        uint64_t    rateLimitEpoch        = 0;
        uint32_t    msgThisSecond         = 0;
        uint64_t    byteThisSecond        = 0;
        uint64_t    banExpiresAt          = 0;
        uint32_t    banCount              = 0;
        uint32_t    decodeErrorCount      = 0;  // Point 7: auto-ban counter
        double      score                 = 100.0;
        bool        isBanned              = false;
        bool        isInbound             = false;
        bool        isReachable           = false;
        bool        handshakeDone         = false;
        size_t      sendQueueDepth        = 0;   // Point 4: backpressure

        // Point 3: token-bucket state — updated under shard write lock
        double      tokenMsgBucket        = 0.0;
        double      tokenByteBucket       = 0.0;
        uint64_t    tokenLastRefillEpoch  = 0;
    };

    // =========================================================================
    // TIME-WINDOWED COUNTER
    // Point 6: 60-second rolling window for QPS and bandwidth
    // =========================================================================
    struct WindowedCounter {
        static constexpr size_t WINDOW_SLOTS = 60;
        std::array<std::atomic<uint64_t>, WINDOW_SLOTS> slots{};
        std::atomic<uint64_t> lastEpoch{0};

        void record(uint64_t value, uint64_t epochSec) noexcept {
            size_t slot = epochSec % WINDOW_SLOTS;
            uint64_t prev = lastEpoch.load(std::memory_order_relaxed);
            if (epochSec != prev) {
                if (lastEpoch.compare_exchange_weak(
                        prev, epochSec,
                        std::memory_order_relaxed))
                    slots[slot].store(0, std::memory_order_relaxed);
            }
            slots[slot].fetch_add(value, std::memory_order_relaxed);
        }

        uint64_t sum() const noexcept {
            uint64_t total = 0;
            for (const auto& s : slots)
                total += s.load(std::memory_order_relaxed);
            return total;
        }

        WindowedCounter() { for (auto& s : slots) s.store(0); }
        WindowedCounter(const WindowedCounter&)            = delete;
        WindowedCounter& operator=(const WindowedCounter&) = delete;
    };

    // =========================================================================
    // METRICS
    // Point 6: full Prometheus export + per decode-error counters
    // =========================================================================
    struct Metrics {
        std::atomic<uint64_t> blocksReceived{0};
        std::atomic<uint64_t> blocksSent{0};
        std::atomic<uint64_t> txReceived{0};
        std::atomic<uint64_t> txSent{0};
        std::atomic<uint64_t> bytesIn{0};
        std::atomic<uint64_t> bytesOut{0};
        std::atomic<uint64_t> connectionsAttempted{0};
        std::atomic<uint64_t> connectionsAccepted{0};
        std::atomic<uint64_t> connectionsRejected{0};
        std::atomic<uint64_t> connectionsDropped{0};
        std::atomic<uint64_t> activePeers{0};
        std::atomic<uint64_t> banEvents{0};
        std::atomic<uint64_t> rateLimitEvents{0};
        std::atomic<uint64_t> byteQuotaEvents{0};
        std::atomic<uint64_t> oversizedFrames{0};
        std::atomic<uint64_t> decodeErrors{0};
        std::atomic<uint64_t> decodeTooShort{0};
        std::atomic<uint64_t> decodeBadMagic{0};
        std::atomic<uint64_t> decodeOversized{0};
        std::atomic<uint64_t> decodePayloadTrunc{0};
        std::atomic<uint64_t> decodeAllocFailed{0};
        std::atomic<uint64_t> decodeVersionMismatch{0};
        std::atomic<uint64_t> decodeBadMsgType{0};
        std::atomic<uint64_t> handshakeFailed{0};
        std::atomic<uint64_t> replayRejected{0};
        std::atomic<uint64_t> badMagicFrames{0};
        std::atomic<uint64_t> tlsHandshakeFailed{0};
        std::atomic<uint64_t> autoBanOnDecodeError{0};
        std::atomic<uint64_t> broadcastAttempts{0};
        std::atomic<uint64_t> broadcastSucceeded{0};
        std::atomic<uint64_t> broadcastFailed{0};
        std::atomic<uint64_t> slowPeerDisconnects{0};
        std::atomic<uint64_t> sendRetries{0};
        std::atomic<uint64_t> sendQueueFull{0};
        std::atomic<uint64_t> pendingQueueRecovered{0};
        std::atomic<uint64_t> peersEvicted{0};

        // Point 6: windowed QPS and bandwidth
        WindowedCounter qpsIn;
        WindowedCounter qpsOut;
        WindowedCounter bwIn;
        WindowedCounter bwOut;

        // Increments total + per-code counter in one call
        void recordDecodeError(codec::DecodeError err) noexcept {
            decodeErrors.fetch_add(1, std::memory_order_relaxed);
            switch (err) {
                case codec::DecodeError::TooShort:
                    decodeTooShort.fetch_add(1,
                        std::memory_order_relaxed); break;
                case codec::DecodeError::BadMagic:
                    decodeBadMagic.fetch_add(1,
                        std::memory_order_relaxed);
                    badMagicFrames.fetch_add(1,
                        std::memory_order_relaxed); break;
                case codec::DecodeError::OversizedFrame:
                    decodeOversized.fetch_add(1,
                        std::memory_order_relaxed);
                    oversizedFrames.fetch_add(1,
                        std::memory_order_relaxed); break;
                case codec::DecodeError::PayloadTrunc:
                    decodePayloadTrunc.fetch_add(1,
                        std::memory_order_relaxed); break;
                case codec::DecodeError::AllocFailed:
                    decodeAllocFailed.fetch_add(1,
                        std::memory_order_relaxed); break;
                case codec::DecodeError::VersionMismatch:
                    decodeVersionMismatch.fetch_add(1,
                        std::memory_order_relaxed); break;
                case codec::DecodeError::BadMessageType:
                    decodeBadMsgType.fetch_add(1,
                        std::memory_order_relaxed); break;
                default: break;
            }
        }

        std::string toPrometheusText() const noexcept;
        std::string toSummaryLine()    const noexcept;

        Metrics()                          = default;
        Metrics(const Metrics&)            = delete;
        Metrics& operator=(const Metrics&) = delete;
    };

    // =========================================================================
    // BROADCAST RESULT
    // =========================================================================
    struct BroadcastResult {
        std::atomic<uint32_t> attempted{0};
        std::atomic<uint32_t> succeeded{0};
        std::atomic<uint32_t> failed{0};
        std::atomic<uint32_t> skippedSlowPeer{0};
        std::atomic<uint32_t> skippedBanned{0};

        BroadcastResult()                              = default;
        BroadcastResult(const BroadcastResult&)        = delete;
        BroadcastResult& operator=(const BroadcastResult&) = delete;
    };

    // =========================================================================
    // CALLBACKS
    // =========================================================================
    using LogFn                 = std::function<void(const LogEntry&)>;
    using BlockReceivedFn       = std::function<void(const Block&,
                                                      const std::string&)>;
    using TransactionReceivedFn = std::function<void(const Transaction&,
                                                      const std::string&)>;
    using PeerConnectedFn       = std::function<void(const PeerInfo&)>;
    using PeerDisconnectedFn    = std::function<void(const std::string&,
                                                      const std::string&)>;
    using ErrorFn               = std::function<void(const std::string&,
                                                      const std::string&)>;
    using PeerScoredFn          = std::function<void(const std::string&,
                                                      double)>;
    using AlertFn               = std::function<void(const std::string&,
                                                      uint64_t,
                                                      uint64_t)>;
    using TLSCertRotateFn       = std::function<bool(std::string&,
                                                      std::string&)>;

    // =========================================================================
    // LIFECYCLE
    // =========================================================================
    explicit Network(Config cfg);
    ~Network();

    Network(const Network&)            = delete;
    Network& operator=(const Network&) = delete;

    bool start()     noexcept;
    void stop()      noexcept;
    bool isRunning() const noexcept;
    bool isLive()    const noexcept;
    bool isReady()   const noexcept;

    // =========================================================================
    // CALLBACKS — install before start()
    // =========================================================================
    void setLogger            (LogFn                 fn) noexcept;
    void onBlockReceived      (BlockReceivedFn        fn) noexcept;
    void onTransactionReceived(TransactionReceivedFn  fn) noexcept;
    void onPeerConnected      (PeerConnectedFn        fn) noexcept;
    void onPeerDisconnected   (PeerDisconnectedFn     fn) noexcept;
    void onError              (ErrorFn                fn) noexcept;
    void onPeerScored         (PeerScoredFn           fn) noexcept;
    void setAlertHandler      (AlertFn                fn) noexcept;
    void setTLSCertRotateHook (TLSCertRotateFn        fn) noexcept;

    // =========================================================================
    // PEER MANAGEMENT
    // =========================================================================
    bool                    connectToPeer  (const std::string& address,
                                             uint16_t port)      noexcept;
    bool                    disconnectPeer (const std::string& peerId)   noexcept;
    bool                    banPeer        (const std::string& peerId)   noexcept;
    bool                    penalizePeer   (const std::string& peerId,
                                             double penalty)              noexcept;
    bool                    rewardPeer     (const std::string& peerId,
                                             double reward)               noexcept;
    bool                    isConnected    (const std::string& address)  const noexcept;
    bool                    isBanned       (const std::string& peerId)   const noexcept;
    std::optional<PeerInfo> getPeer        (const std::string& peerId)   const noexcept;
    std::vector<PeerInfo>   getPeers       ()                            const noexcept;
    std::vector<PeerInfo>   getActivePeers ()                            const noexcept;
    size_t                  peerCount      ()                            const noexcept;
    size_t                  activePeerCount()                            const noexcept;

    void savePeers() const noexcept;
    void loadPeers()       noexcept;

    // =========================================================================
    // BROADCASTING
    // =========================================================================
    BroadcastResult broadcastBlock(const Block& block)                noexcept;
    BroadcastResult broadcastTransaction(const Transaction& tx)       noexcept;
    BroadcastResult broadcastTransactionBatch(
        const std::vector<Transaction>& txs)                          noexcept;
    BroadcastResult broadcastToPeer(const std::string&   peerId,
                                     codec::MessageType   type,
                                     std::vector<uint8_t> payload)    noexcept;

    // =========================================================================
    // DYNAMIC TUNING — safe from any thread at any time
    // =========================================================================
    void setRateLimit      (uint32_t maxMsgPerSec,
                             uint64_t maxBytesPerSec)  noexcept;
    void setMaxPeers       (size_t maxPeers)            noexcept;
    void setWorkerCounts   (size_t inbound,
                             size_t outbound)           noexcept;
    void setLogLevel       (int minLevel)               noexcept;
    void setAlertThresholds(const AlertThresholds& t)   noexcept;
    bool rotateTLSCertificate()                         noexcept;

    // =========================================================================
    // METRICS AND OBSERVABILITY
    // =========================================================================
    const Metrics& metrics()           const noexcept;
    std::string    getMetricsSummary() const noexcept;
    std::string    getPrometheusText() const noexcept;
    void           resetMetrics()            noexcept;
    void           dumpMetricsToFile()  const noexcept;

    // =========================================================================
    // TESTING AND FUZZING
    // =========================================================================
    static bool codecSelfTest() noexcept;
    static bool fuzzFrame(const uint8_t* data, size_t len) noexcept;

    // =========================================================================
    // STATIC HELPERS
    // =========================================================================
    static uint64_t    nowSecs()                              noexcept;
    static uint64_t    nowMs()                                noexcept;
    static std::string peerKey(const std::string& address,
                                uint16_t port)                 noexcept;
    static size_t      resolveWorkerCount(size_t configured)   noexcept;

private:

    // =========================================================================
    // SHARDED PEER MAP
    // Point 7: shardCount_ is atomic — maybeGrowShards() updates it safely.
    // shardFor() reads shardCount_ with acquire ordering.
    // =========================================================================
    struct PeerShard {
        mutable std::shared_mutex                 mu;
        std::unordered_map<std::string, PeerInfo> peers;
    };

    std::vector<std::unique_ptr<PeerShard>> shards_;
    std::atomic<size_t>                     shardCount_{1};

    PeerShard&       shardFor(const std::string& key)        noexcept;
    const PeerShard& shardFor(const std::string& key)  const noexcept;
    static size_t    shardIndex(const std::string& key,
                                 size_t count)                noexcept;

    // Point 7: safe shard growth under all write locks held in index order
    void maybeGrowShards() noexcept;

    // =========================================================================
    // PIMPL — hides WorkerPool, ConnectionPool, ReconnectTracker, threads
    // Guaranteed non-null after construction.
    // =========================================================================
    struct Impl;
    std::unique_ptr<Impl> impl_;

    Config          cfg_;
    Metrics         metrics_;
    AlertThresholds alertThresholds_;

    // =========================================================================
    // PER-TYPE CALLBACK MUTEXES
    // Each callback type has its own mutex — they never block each other.
    // =========================================================================
    mutable std::mutex logMu_;
    mutable std::mutex blockCbMu_;
    mutable std::mutex txCbMu_;
    mutable std::mutex peerConnMu_;
    mutable std::mutex peerDiscMu_;
    mutable std::mutex errorMu_;
    mutable std::mutex scoreMu_;
    mutable std::mutex alertMu_;
    mutable std::mutex tlsRotateMu_;

    LogFn                 logFn_;
    BlockReceivedFn       onBlockReceivedFn_;
    TransactionReceivedFn onTransactionReceivedFn_;
    PeerConnectedFn       onPeerConnectedFn_;
    PeerDisconnectedFn    onPeerDisconnectedFn_;
    ErrorFn               errorFn_;
    PeerScoredFn          onPeerScoredFn_;
    AlertFn               alertFn_;
    TLSCertRotateFn       tlsCertRotateFn_;

    std::atomic<bool> running_{false};
    std::atomic<int>  minLogLevel_{0};

    // =========================================================================
    // DYNAMIC TUNING ATOMICS
    // Point 3: msg and byte limits adjustable at runtime
    // Initialized in constructor — safe to read before start()
    // =========================================================================
    std::atomic<uint32_t> dynMaxMsgPerSec_       {200};
    std::atomic<uint64_t> dynMaxBytesPerSec_     {10ULL * 1024 * 1024};
    std::atomic<size_t>   dynMaxPeers_           {1000};
    std::atomic<size_t>   dynInboundWorkers_     {0};
    std::atomic<size_t>   dynOutboundWorkers_    {0};
    std::atomic<uint32_t> dynMaxDecodeErrBan_    {10};

    // Point 6: alert threshold atomics for lock-free hot-path checking
    std::atomic<uint64_t> alertMaxBanPerMin_     {50};
    std::atomic<uint64_t> alertMaxFailSendPerMin_{100};
    std::atomic<uint64_t> alertMaxDecErrPerMin_  {200};

    // =========================================================================
    // INTERNAL HELPERS — all implemented in network.cpp
    // =========================================================================
    void slog(int level, const std::string& component,
              const std::string& peerId,
              const std::string& msg)              const noexcept;
    void slog(int level, const std::string& component,
              const std::string& msg)              const noexcept;

    // Point 6: log decode failure and increment per-code metric
    void logDecodeFailure(const std::string& peerId,
                           codec::DecodeError  err)    const noexcept;

    // Point 6: fire alertFn_ if value >= threshold
    void checkAlert(const std::string& metric,
                     uint64_t           value,
                     uint64_t           threshold)     const noexcept;

    // Point 2: TLS validation at start()
    bool validateTLSConfig()                           const noexcept;

    // Point 3: token-bucket — called under shard write lock
    void refillTokenBucket    (PeerInfo& peer,
                                uint64_t nowSec)             noexcept;
    bool checkRateLimitLocked (PeerInfo& peer)               noexcept;
    bool checkByteQuotaLocked (PeerInfo& peer,
                                size_t bytes)                 noexcept;

    // Point 1: deliver with exponential backoff via ReconnectTracker
    bool deliverToPeer(const std::string&          peerId,
                        codec::MessageType           type,
                        const std::vector<uint8_t>& payload) noexcept;

    BroadcastResult broadcastPayload(codec::MessageType    type,
                                      std::vector<uint8_t>  payload,
                                      const std::string&    label)  noexcept;

    // Listener
    bool bindListener()   noexcept;
    void runListener()    noexcept;

    // Point 8: background loop threads
    void runHealthCheck()       noexcept;
    void runPingLoop()          noexcept;
    void runMetricsDumper()     noexcept;
    void runAlertChecker()      noexcept;
    void runHealthServer()      noexcept;
    void runPrometheusServer()  noexcept;

    // Frame handling
    void handleInbound (int fd,
                         const std::string& peerAddr)        noexcept;
    void dispatchFrame (const codec::Frame& frame,
                         const std::string& peerAddr)        noexcept;

    // Peer lifecycle
    void evictStalePeers()                                   noexcept;
    void unbanExpiredPeers()                                  noexcept;
    void sendPings()                                          noexcept;
    void disconnectSlowPeers()                                noexcept;

    // Point 7: auto-ban on repeated decode errors
    void maybeAutoBanPeer(const std::string& peerId)          noexcept;

    // Scoring — calls onPeerScoredFn_ after change
    void applyScoreChange(const std::string& peerId,
                           double delta,
                           bool   isBanCheck)                 noexcept;

    // Point 5: crash recovery with checksum
    void recoverPendingQueue() noexcept;
    void persistPendingQueue() noexcept;
};
