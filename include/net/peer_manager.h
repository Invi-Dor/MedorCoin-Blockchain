#pragma once

#include "block.h"
#include "transaction.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace net {

// =============================================================================
// LOG ENTRY
// =============================================================================
struct LogEntry {
    int         level       = 0;
    std::string component;
    std::string peerId;
    std::string message;
    uint64_t    timestampMs = 0;
};

// =============================================================================
// PEER INFO
// =============================================================================
struct PeerInfo {
    std::string id;
    std::string address;
    uint16_t    port              = 0;
    uint32_t    version           = 0;
    uint64_t    connectedAt       = 0;
    uint64_t    lastSeenAt        = 0;
    uint64_t    lastPingSentAt    = 0;
    uint64_t    lastPongAt        = 0;
    uint64_t    messagesSent      = 0;
    uint64_t    messagesRecv      = 0;
    uint64_t    bytesRecv         = 0;
    uint64_t    bytesSent         = 0;
    uint64_t    banExpiresAt      = 0;
    uint32_t    banCount          = 0;
    uint32_t    decodeErrorCount  = 0;
    double      score             = 100.0;
    bool        isBanned          = false;
    bool        isInbound         = false;
    bool        isReachable       = false;
    bool        handshakeDone     = false;
    size_t      sendQueueDepth    = 0;
    double      tokenMsgBucket    = 0.0;
    double      tokenByteBucket   = 0.0;
    uint64_t    tokenLastRefill   = 0;
    bool        dirty             = false;
};

// =============================================================================
// CONFIG
// =============================================================================
struct PeerManagerConfig {
    size_t      peerMapShards              = 64;
    bool        adaptiveSharding           = true;
    size_t      maxPeers                   = 1000;
    uint64_t    peerTimeoutSecs            = 120;
    uint64_t    banDurationSecs            = 3600;
    uint32_t    handshakeTimeoutMs         = 5000;
    uint32_t    maxMsgPerSecPerPeer        = 200;
    uint64_t    maxBytesPerSecPerPeer      = 10ULL * 1024 * 1024;
    uint32_t    tokenBucketBurstMsg        = 400;
    uint64_t    tokenBucketBurstBytes      = 20ULL * 1024 * 1024;
    uint32_t    maxDecodeErrorsBeforeBan   = 10;
    uint32_t    maxBanCount                = 1000;
    uint32_t    protocolVersion            = 1;
    uint32_t    minPeerVersion             = 1;
    uint32_t    maxPeerVersion             = 1;
    std::string networkMagic               = "MEDOR";
    std::string nodeId;
    std::string peerStorePath              = "data/peers.dat";
    bool        incrementalSave            = true;
    uint64_t    persistenceIntervalSec     = 30;
    uint64_t    cleanupIntervalSec         = 600;
    uint64_t    metricsIntervalSec         = 60;
    std::string metricsExportPath;
    size_t      asyncLogBufferSize         = 4096;
    size_t      backgroundWorkers          = 2;
};

// =============================================================================
// METRICS
// =============================================================================
struct PeerManagerMetrics {
    std::atomic<uint64_t> activePeers{0};
    std::atomic<uint64_t> totalConnected{0};
    std::atomic<uint64_t> totalEvicted{0};
    std::atomic<uint64_t> banEvents{0};
    std::atomic<uint64_t> autoBanDecodeError{0};
    std::atomic<uint64_t> rateLimitEvents{0};
    std::atomic<uint64_t> byteQuotaEvents{0};
    std::atomic<uint64_t> decodeErrors{0};
    std::atomic<uint64_t> handshakeFailed{0};
    std::atomic<uint64_t> handshakeSuccess{0};
    std::atomic<uint64_t> replayRejected{0};
    std::atomic<uint64_t> peersEvicted{0};
    std::atomic<uint64_t> shardGrowths{0};
    std::atomic<uint64_t> saveCount{0};
    std::atomic<uint64_t> loadCount{0};
    std::atomic<uint64_t> incrementalSaveCount{0};

    struct Window {
        static constexpr size_t SLOTS = 60;
        std::array<std::atomic<uint64_t>, SLOTS> s{};
        std::atomic<uint64_t> epoch{0};

        Window() { for (auto& x : s) x.store(0); }

        void record(uint64_t v, uint64_t sec) noexcept {
            size_t slot = sec % SLOTS;
            uint64_t prev = epoch.load(std::memory_order_relaxed);
            if (sec != prev &&
                epoch.compare_exchange_weak(prev, sec,
                    std::memory_order_relaxed))
                s[slot].store(0, std::memory_order_relaxed);
            s[slot].fetch_add(v, std::memory_order_relaxed);
        }

        uint64_t sum() const noexcept {
            uint64_t t = 0;
            for (const auto& x : s)
                t += x.load(std::memory_order_relaxed);
            return t;
        }

        Window(const Window&)            = delete;
        Window& operator=(const Window&) = delete;
    };

    Window msgIn;
    Window msgOut;
    Window bytesIn;
    Window bytesOut;

    std::string toPrometheusText() const noexcept;

    PeerManagerMetrics()                                     = default;
    PeerManagerMetrics(const PeerManagerMetrics&)            = delete;
    PeerManagerMetrics& operator=(const PeerManagerMetrics&) = delete;
};

// =============================================================================
// PEER MANAGER
// =============================================================================
class PeerManager {
public:
    using LogFn              = std::function<void(const LogEntry&)>;
    using PeerConnectedFn    = std::function<void(const PeerInfo&)>;
    using PeerDisconnectedFn = std::function<void(const std::string&,
                                                   const std::string&)>;
    using PeerScoredFn       = std::function<void(const std::string&,
                                                   double)>;
    using AlertFn            = std::function<void(const std::string&,
                                                   uint64_t, uint64_t)>;

    explicit PeerManager(PeerManagerConfig cfg);
    ~PeerManager();

    PeerManager(const PeerManager&)            = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    void start()     noexcept;
    void stop()      noexcept;
    bool isRunning() const noexcept;

    void reloadConfig(const PeerManagerConfig& newCfg) noexcept;

    void setLogger          (LogFn fn)              noexcept;
    void onPeerConnected    (PeerConnectedFn fn)    noexcept;
    void onPeerDisconnected (PeerDisconnectedFn fn) noexcept;
    void onPeerScored       (PeerScoredFn fn)       noexcept;
    void setAlertHandler    (AlertFn fn)             noexcept;

    bool                    addPeer    (const PeerInfo& info)        noexcept;
    bool                    removePeer (const std::string& id)       noexcept;
    bool                    banPeer    (const std::string& id)       noexcept;
    bool                    penalizePeer(const std::string& id,
                                          double penalty)             noexcept;
    bool                    rewardPeer  (const std::string& id,
                                          double reward)              noexcept;
    bool                    hasPeer    (const std::string& id) const noexcept;
    bool                    isBanned   (const std::string& id) const noexcept;
    std::optional<PeerInfo> getPeer    (const std::string& id) const noexcept;
    std::vector<PeerInfo>   getAllPeers    () const noexcept;
    std::vector<PeerInfo>   getActivePeers() const noexcept;
    size_t                  peerCount     () const noexcept;

    bool doHandshake       (const std::string& peerId, int fd)       noexcept;
    bool checkRateLimit    (const std::string& id, size_t msgBytes)  noexcept;
    void recordDecodeError (const std::string& id)                   noexcept;

    // Fix 3: two-bucket dedup
    bool markSeen          (const std::string& msgId)                noexcept;
    void cleanupSeenMessages()                                        noexcept;

    void evictStalePeers   () noexcept;
    void evictLowScorePeers() noexcept;
    void unbanExpiredPeers () noexcept;

    void savePeers() const noexcept;
    void loadPeers()       noexcept;

    const PeerManagerMetrics& metrics()         const noexcept;
    std::string               getPrometheusText() const noexcept;
    void                      exportMetrics()          noexcept;

    void setRateLimit               (uint32_t maxMsg, uint64_t maxBytes) noexcept;
    void setMaxDecodeErrorsBeforeBan(uint32_t n)                         noexcept;
    void setMaxPeers                (size_t n)                           noexcept;

    static uint64_t    nowSecs() noexcept;
    static uint64_t    nowMs()   noexcept;
    static std::string peerKey(const std::string& address,
                                uint16_t port)           noexcept;

private:
    // =========================================================================
    // PEER SHARD
    // =========================================================================
    struct PeerShard {
        mutable std::shared_mutex                 mu;
        std::unordered_map<std::string, PeerInfo> peers;

        bool hasPeer(const std::string& id) const noexcept {
            return peers.count(id) > 0;
        }
        PeerInfo* find(const std::string& id) noexcept {
            auto it = peers.find(id);
            return it != peers.end() ? &it->second : nullptr;
        }
        const PeerInfo* find(const std::string& id) const noexcept {
            auto it = peers.find(id);
            return it != peers.end() ? &it->second : nullptr;
        }
    };

    // =========================================================================
    // ASYNC LOG QUEUE
    // Fix 4: async logging so hot paths never block on I/O or callbacks
    // =========================================================================
    struct AsyncLogQueue {
        mutable std::mutex      mu;
        std::condition_variable cv;
        std::vector<LogEntry>   queue;
        std::atomic<bool>       stopped{false};
        std::thread             worker;
        LogFn                   sink;
        size_t                  maxSize = 4096;

        void push(LogEntry e) noexcept {
            std::lock_guard<std::mutex> lk(mu);
            if (queue.size() >= maxSize) return;
            queue.push_back(std::move(e));
            cv.notify_one();
        }

        void run() {
            while (!stopped.load()) {
                std::vector<LogEntry> batch;
                {
                    std::unique_lock<std::mutex> lk(mu);
                    cv.wait_for(lk, std::chrono::milliseconds(100),
                        [this]() {
                            return !queue.empty() || stopped.load();
                        });
                    batch.swap(queue);
                }
                for (auto& entry : batch) {
                    if (sink) {
                        try { sink(entry); }
                        catch (...) {}
                    } else if (entry.level >= 1) {
                        std::cerr << "[" << entry.component << "] "
                                  << entry.message << "\n";
                    }
                }
            }
            std::vector<LogEntry> remaining;
            {
                std::lock_guard<std::mutex> lk(mu);
                remaining.swap(queue);
            }
            for (auto& entry : remaining) {
                if (sink) try { sink(entry); } catch (...) {}
            }
        }
    };

    // =========================================================================
    // WORKER POOL
    // Fix 2: shared pool for all background tasks
    // =========================================================================
    struct WorkerPool {
        std::vector<std::thread>            workers;
        std::mutex                          mu;
        std::condition_variable             cv;
        std::vector<std::function<void()>>  queue;
        std::atomic<bool>                   stopped{false};

        void start(size_t n) {
            for (size_t i = 0; i < n; i++) {
                workers.emplace_back([this]() {
                    while (true) {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lk(mu);
                            cv.wait(lk, [this]() {
                                return stopped.load() || !queue.empty();
                            });
                            if (stopped.load() && queue.empty()) return;
                            task = std::move(queue.front());
                            queue.erase(queue.begin());
                        }
                        try { task(); } catch (...) {}
                    }
                });
            }
        }

        void post(std::function<void()> fn) noexcept {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (stopped.load()) return;
                queue.push_back(std::move(fn));
            }
            cv.notify_one();
        }

        void stop() noexcept {
            stopped.store(true);
            cv.notify_all();
            for (auto& t : workers)
                if (t.joinable()) t.join();
        }
    };

    // =========================================================================
    // MEMBERS
    // =========================================================================
    std::vector<std::unique_ptr<PeerShard>> shards_;
    std::atomic<size_t>                     shardCount_{1};

    // Fix 1: atomic flag prevents concurrent shard growth
    std::atomic<bool>                       growthInProgress_{false};

    PeerShard&       shardFor(const std::string& id)       noexcept;
    const PeerShard& shardFor(const std::string& id) const noexcept;
    static size_t    shardIndex(const std::string& id,
                                 size_t count)               noexcept;
    void maybeGrowShards() noexcept;

    PeerManagerConfig     cfg_;
    mutable std::mutex    cfgMu_;
    PeerManagerMetrics    metrics_;

    mutable std::mutex logMu_;
    mutable std::mutex peerConnMu_;
    mutable std::mutex peerDiscMu_;
    mutable std::mutex scoreMu_;
    mutable std::mutex alertMu_;

    LogFn              logFn_;
    PeerConnectedFn    onPeerConnectedFn_;
    PeerDisconnectedFn onPeerDisconnectedFn_;
    PeerScoredFn       onPeerScoredFn_;
    AlertFn            alertFn_;

    AsyncLogQueue logQueue_;
    WorkerPool    bgPool_;

    std::thread             timerThread_;
    std::atomic<bool>       timerStopped_{false};
    std::mutex              timerMu_;
    std::condition_variable timerCv_;

    void runTimerLoop()       noexcept;
    void startBackgroundTasks();

    // Fix 3: two-bucket message dedup — no full protection gap on rotation
    mutable std::mutex              seenMu_;
    std::unordered_set<std::string> seenActive_;
    std::unordered_set<std::string> seenStale_;
    static constexpr size_t         SEEN_MAX = 500000;

    // Incremental save dirty tracking
    mutable std::mutex              dirtyMu_;
    std::unordered_set<std::string> dirtyIds_;
    void markDirty(const std::string& id) noexcept;

    std::atomic<bool> running_{false};

    void slog(int level, const std::string& component,
              const std::string& peerId,
              const std::string& msg) const noexcept;
    void slog(int level, const std::string& component,
              const std::string& msg) const noexcept;

    void applyScore   (const std::string& id, double delta) noexcept;
    void refillBucket (PeerInfo& peer, uint64_t nowSec)      noexcept;

    // Fix 4: type-safe callback invoker with structured error reporting
    template<typename Fn, typename... Args>
    void invokeCallback(const char*        callbackName,
                         const std::string& peerId,
                         Fn&                fn,
                         Args&&...          args) noexcept;

    std::atomic<uint32_t> dynMaxMsgPerSec_   {200};
    std::atomic<uint64_t> dynMaxBytesPerSec_ {10ULL * 1024 * 1024};
    std::atomic<uint32_t> dynMaxDecodeErrBan_{10};
    std::atomic<size_t>   dynMaxPeers_       {1000};
};

} // namespace net
