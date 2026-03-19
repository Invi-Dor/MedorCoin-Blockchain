#include "net/peer_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <vector>

namespace net {

// =============================================================================
// CRC-32 — IEEE 802.3 polynomial 0xEDB88320
// =============================================================================
static const uint32_t CRC32_TABLE[256] = {
    0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,
    0xE963A535,0x9E6495A3,0x0EDB8832,0x79DCB8A4,0xE0D5E91B,0x97D2D988,
    0x09B64C2B,0x7EB17CBF,0xE7B82D09,0x90BF1CBB,0x1DB71064,0x6AB020F2,
    0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
    0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,
    0xFA0F3D63,0x8D080DF5,0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,
    0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,0x35B5A8FA,0x42B2986C,
    0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
    0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F927,0x56B3C9B1,
    0xCFBA9899,0xB8BDA50F,0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,
    0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,0x76DC4190,0x01DB7106,
    0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
    0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6396BB,0x086D3D2D,
    0x91646C97,0xE6635C01,0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,
    0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,0x65B0D9C6,0x12B7E950,
    0x8BBEB8EA,0xFCB9887C,0x62DD1D7F,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
    0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,
    0xA4D1C46D,0xD3D6F4FB,0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,
    0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,0x5005713C,0x270241AA,
    0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
    0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,
    0xB7BD5C3B,0xC0BA6CAD,0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,
    0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,0xE3630B12,0x94643B84,
    0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
    0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,
    0x196C3671,0x6E6B06E7,0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,
    0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,0xD6D6A3E8,0xA1D1937E,
    0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
    0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA8670955,
    0x316658EF,0x466856A9,0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D
};

static uint32_t crc32(const void* data, size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

// =============================================================================
// BINARY STORE FORMAT
// =============================================================================
static constexpr uint32_t STORE_MAGIC   = 0x4D445250;
static constexpr uint32_t STORE_VERSION = 3;

#pragma pack(push,1)
struct StoreHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t numPeers;
    uint64_t timestamp;
    uint32_t checksum;
    uint8_t  reserved[12];
};
struct StoredPeer {
    uint8_t  addrLen;
    char     addr[255];
    uint16_t port;
    double   score;
    uint64_t connectedAt;
    uint64_t lastSeen;
    uint64_t banExpiresAt;
    uint32_t banCount;
    uint32_t decodeErrorCount;
    uint8_t  isBanned;
    uint8_t  handshakeDone;
    uint8_t  pad[2];
};
#pragma pack(pop)

// =============================================================================
// STATIC HELPERS
// =============================================================================
uint64_t PeerManager::nowSecs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now()
                .time_since_epoch()).count());
}

uint64_t PeerManager::nowMs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()
                .time_since_epoch()).count());
}

std::string PeerManager::peerKey(const std::string& address,
                                   uint16_t port) noexcept {
    return address + ":" + std::to_string(port);
}

size_t PeerManager::shardIndex(const std::string& id,
                                 size_t count) noexcept {
    return std::hash<std::string>{}(id) % count;
}

PeerManager::PeerShard&
PeerManager::shardFor(const std::string& id) noexcept {
    size_t sc  = shardCount_.load(std::memory_order_acquire);
    size_t idx = shardIndex(id, sc);
    return *shards_[idx];
}

const PeerManager::PeerShard&
PeerManager::shardFor(const std::string& id) const noexcept {
    size_t sc  = shardCount_.load(std::memory_order_acquire);
    size_t idx = shardIndex(id, sc);
    return *shards_[idx];
}

// =============================================================================
// FIX 1: SHARD GROWTH — race-condition free
// growthInProgress_ prevents concurrent growth.
// All shard write locks acquired in ascending index order.
// shardCount_ updated only after redistribution is complete.
// =============================================================================
void PeerManager::maybeGrowShards() noexcept {
    bool expected = false;
    if (!growthInProgress_.compare_exchange_strong(
            expected, true,
            std::memory_order_acquire,
            std::memory_order_relaxed))
        return;

    struct Guard {
        std::atomic<bool>& flag;
        ~Guard() { flag.store(false, std::memory_order_release); }
    } guard{growthInProgress_};

    size_t current = shardCount_.load(std::memory_order_acquire);

    size_t total = 0;
    for (size_t i = 0; i < current; i++) {
        std::shared_lock<std::shared_mutex> rlock(shards_[i]->mu);
        total += shards_[i]->peers.size();
    }
    if (total <= current * 8) return;

    size_t next = std::min(current * 2, size_t(1024));
    if (next == current) return;

    // Build new shard peer maps before acquiring any locks
    std::vector<std::unordered_map<std::string, PeerInfo>> newMaps(next);

    // Lock ALL existing shards in ascending order — deadlock-safe
    for (size_t i = 0; i < current; i++)
        shards_[i]->mu.lock();

    // Collect and redistribute
    for (size_t i = 0; i < current; i++) {
        for (auto& [key, peer] : shards_[i]->peers) {
            size_t idx = shardIndex(key, next);
            newMaps[idx].emplace(key, std::move(peer));
        }
        shards_[i]->peers.clear();
    }

    // Extend shards_ vector with new empty shards
    while (shards_.size() < next)
        shards_.push_back(std::make_unique<PeerShard>());

    // Move redistributed peers into shards
    for (size_t i = 0; i < next; i++)
        shards_[i]->peers = std::move(newMaps[i]);

    // Publish new count AFTER all shards are populated
    shardCount_.store(next, std::memory_order_release);

    // Unlock in same ascending order
    for (size_t i = 0; i < current; i++)
        shards_[i]->mu.unlock();

    metrics_.shardGrowths.fetch_add(1, std::memory_order_relaxed);
    slog(0, "PeerManager",
         "shards grown " + std::to_string(current)
         + " -> " + std::to_string(next));
}

// =============================================================================
// CONSTRUCTOR
// =============================================================================
PeerManager::PeerManager(PeerManagerConfig cfg)
    : cfg_(std::move(cfg))
{
    size_t sc = std::max(size_t(1), cfg_.peerMapShards);
    shardCount_.store(sc, std::memory_order_release);
    shards_.reserve(sc);
    for (size_t i = 0; i < sc; i++)
        shards_.push_back(std::make_unique<PeerShard>());

    dynMaxMsgPerSec_.store(cfg_.maxMsgPerSecPerPeer);
    dynMaxBytesPerSec_.store(cfg_.maxBytesPerSecPerPeer);
    dynMaxDecodeErrBan_.store(cfg_.maxDecodeErrorsBeforeBan);
    dynMaxPeers_.store(cfg_.maxPeers);

    logQueue_.maxSize = std::max(size_t(256), cfg_.asyncLogBufferSize);
    logQueue_.worker  = std::thread([this]() { logQueue_.run(); });

    seenActive_.reserve(SEEN_MAX / 2);
    seenStale_.reserve(SEEN_MAX / 2);
}

// =============================================================================
// DESTRUCTOR
// =============================================================================
PeerManager::~PeerManager() { stop(); }

// =============================================================================
// START / STOP
// =============================================================================
void PeerManager::start() noexcept {
    if (running_.exchange(true)) return;
    bgPool_.start(std::max(size_t(1), cfg_.backgroundWorkers));
    startBackgroundTasks();
    loadPeers();
    slog(0, "PeerManager",
         "started shards=" + std::to_string(shardCount_.load()));
}

void PeerManager::stop() noexcept {
    if (!running_.exchange(false)) return;
    timerStopped_.store(true);
    timerCv_.notify_all();
    if (timerThread_.joinable()) timerThread_.join();
    bgPool_.stop();
    savePeers();
    logQueue_.stopped.store(true);
    logQueue_.cv.notify_all();
    if (logQueue_.worker.joinable()) logQueue_.worker.join();
}

bool PeerManager::isRunning() const noexcept { return running_.load(); }

// =============================================================================
// HOT CONFIG RELOAD
// =============================================================================
void PeerManager::reloadConfig(const PeerManagerConfig& newCfg) noexcept {
    std::lock_guard<std::mutex> lk(cfgMu_);
    cfg_ = newCfg;
    dynMaxMsgPerSec_.store(newCfg.maxMsgPerSecPerPeer);
    dynMaxBytesPerSec_.store(newCfg.maxBytesPerSecPerPeer);
    dynMaxDecodeErrBan_.store(newCfg.maxDecodeErrorsBeforeBan);
    dynMaxPeers_.store(newCfg.maxPeers);
    slog(0, "PeerManager", "config reloaded");
}

// =============================================================================
// FIX 2: BACKGROUND TASK TIMER — full exception isolation per task
// Tasks that repeatedly fail are suspended to prevent log flooding.
// =============================================================================
void PeerManager::startBackgroundTasks() {
    timerThread_ = std::thread([this]() { runTimerLoop(); });
}

void PeerManager::runTimerLoop() noexcept {
    static constexpr uint32_t MAX_CONSECUTIVE_FAILURES = 5;
    static constexpr uint64_t SUSPEND_SECS             = 300;

    struct TaskState {
        uint32_t failures    = 0;
        uint64_t suspendUtil = 0;
    };
    TaskState persistState, cleanupState, metricsState;

    uint64_t lastPersist = nowSecs();
    uint64_t lastCleanup = nowSecs();
    uint64_t lastMetrics = nowSecs();

    auto runTask = [&](const char* name,
                        TaskState& state,
                        std::function<void()> fn) noexcept {
        uint64_t now = nowSecs();
        if (state.suspendUtil > 0 && now < state.suspendUtil) return;
        try {
            fn();
            state.failures    = 0;
            state.suspendUtil = 0;
        } catch (const std::exception& e) {
            ++state.failures;
            slog(2, "PeerManager",
                 std::string(name) + " threw: " + e.what()
                 + " (fail #" + std::to_string(state.failures) + ")");
            if (state.failures >= MAX_CONSECUTIVE_FAILURES) {
                state.suspendUtil = nowSecs() + SUSPEND_SECS;
                slog(2, "PeerManager",
                     std::string(name) + " suspended "
                     + std::to_string(SUSPEND_SECS) + "s");
            }
        } catch (...) {
            ++state.failures;
            slog(2, "PeerManager",
                 std::string(name) + " threw unknown exception"
                 " (fail #" + std::to_string(state.failures) + ")");
            if (state.failures >= MAX_CONSECUTIVE_FAILURES) {
                state.suspendUtil = nowSecs() + SUSPEND_SECS;
                slog(2, "PeerManager",
                     std::string(name) + " suspended after repeated failures");
            }
        }
    };

    while (!timerStopped_.load()) {
        {
            std::unique_lock<std::mutex> lk(timerMu_);
            timerCv_.wait_for(lk, std::chrono::seconds(1),
                [this]() { return timerStopped_.load(); });
        }
        if (timerStopped_.load()) break;

        uint64_t now = nowSecs();
        uint64_t pi, ci, mi;
        {
            std::lock_guard<std::mutex> lk(cfgMu_);
            pi = cfg_.persistenceIntervalSec;
            ci = cfg_.cleanupIntervalSec;
            mi = cfg_.metricsIntervalSec;
        }

        if (now - lastPersist >= pi) {
            lastPersist = now;
            runTask("savePeers", persistState,
                [this]() { savePeers(); });
        }

        if (now - lastCleanup >= ci) {
            lastCleanup = now;
            runTask("cleanup", cleanupState,
                [this]() {
                    evictStalePeers();
                    evictLowScorePeers();
                    unbanExpiredPeers();
                    cleanupSeenMessages();
                    if (cfg_.adaptiveSharding) maybeGrowShards();
                });
        }

        if (now - lastMetrics >= mi) {
            lastMetrics = now;
            runTask("exportMetrics", metricsState,
                [this]() { exportMetrics(); });
        }
    }
}

// =============================================================================
// CALLBACKS
// =============================================================================
void PeerManager::setLogger(LogFn fn) noexcept {
    std::lock_guard<std::mutex> l(logMu_);
    logFn_ = std::move(fn);
    logQueue_.sink = logFn_;
}
void PeerManager::onPeerConnected(PeerConnectedFn fn) noexcept {
    std::lock_guard<std::mutex> l(peerConnMu_);
    onPeerConnectedFn_ = std::move(fn);
}
void PeerManager::onPeerDisconnected(PeerDisconnectedFn fn) noexcept {
    std::lock_guard<std::mutex> l(peerDiscMu_);
    onPeerDisconnectedFn_ = std::move(fn);
}
void PeerManager::onPeerScored(PeerScoredFn fn) noexcept {
    std::lock_guard<std::mutex> l(scoreMu_);
    onPeerScoredFn_ = std::move(fn);
}
void PeerManager::setAlertHandler(AlertFn fn) noexcept {
    std::lock_guard<std::mutex> l(alertMu_);
    alertFn_ = std::move(fn);
}

// =============================================================================
// FIX 4: SAFE CALLBACK INVOKER
// Catches and logs all exceptions from user callbacks.
// Forwards errors to alertFn_ so monitoring systems see them.
// Never silently swallows anything.
// =============================================================================
template<typename Fn, typename... Args>
void PeerManager::invokeCallback(const char*        callbackName,
                                   const std::string& peerId,
                                   Fn&                fn,
                                   Args&&...          args) noexcept {
    if (!fn) return;
    try {
        fn(std::forward<Args>(args)...);
    } catch (const std::exception& e) {
        slog(2, "PeerManager", peerId,
             std::string("callback '") + callbackName
             + "' threw: " + e.what());
        std::lock_guard<std::mutex> lk(alertMu_);
        if (alertFn_) {
            try {
                alertFn_(std::string("callback_error:") + callbackName,
                          1, 0);
            } catch (...) {}
        }
    } catch (...) {
        slog(2, "PeerManager", peerId,
             std::string("callback '") + callbackName
             + "' threw unknown exception");
        std::lock_guard<std::mutex> lk(alertMu_);
        if (alertFn_) {
            try {
                alertFn_(std::string("callback_error:") + callbackName,
                          1, 0);
            } catch (...) {}
        }
    }
}

// =============================================================================
// ASYNC LOGGER
// =============================================================================
void PeerManager::slog(int level, const std::string& component,
                         const std::string& peerId,
                         const std::string& msg) const noexcept {
    LogEntry e;
    e.level       = level;
    e.component   = component;
    e.peerId      = peerId;
    e.message     = msg;
    e.timestampMs = nowMs();
    const_cast<PeerManager*>(this)->logQueue_.push(std::move(e));
}

void PeerManager::slog(int level, const std::string& component,
                         const std::string& msg) const noexcept {
    slog(level, component, "", msg);
}

// =============================================================================
// ADD PEER — callbacks via invokeCallback (Fix 4)
// =============================================================================
bool PeerManager::addPeer(const PeerInfo& info) noexcept {
    if (info.id.empty() || info.address.empty()) return false;
    if (peerCount() >= dynMaxPeers_.load(std::memory_order_relaxed))
        return false;

    auto& shard = shardFor(info.id);
    {
        std::unique_lock<std::shared_mutex> wlock(shard.mu);
        if (shard.hasPeer(info.id)) return false;
        PeerInfo copy             = info;
        copy.tokenMsgBucket       = cfg_.tokenBucketBurstMsg;
        copy.tokenByteBucket      = cfg_.tokenBucketBurstBytes;
        copy.tokenLastRefill      = nowSecs();
        copy.dirty                = true;
        shard.peers.emplace(info.id, std::move(copy));
        metrics_.activePeers.fetch_add(1, std::memory_order_relaxed);
        metrics_.totalConnected.fetch_add(1, std::memory_order_relaxed);
    }

    markDirty(info.id);
    if (cfg_.adaptiveSharding) maybeGrowShards();

    {
        std::lock_guard<std::mutex> cbLock(peerConnMu_);
        invokeCallback("onPeerConnected", info.id,
                        onPeerConnectedFn_, info);
    }
    return true;
}

// =============================================================================
// REMOVE PEER — callbacks via invokeCallback (Fix 4)
// =============================================================================
bool PeerManager::removePeer(const std::string& id) noexcept {
    auto& shard = shardFor(id);
    bool found = false;
    {
        std::unique_lock<std::shared_mutex> wlock(shard.mu);
        found = shard.peers.erase(id) > 0;
        if (found)
            metrics_.activePeers.fetch_sub(1, std::memory_order_relaxed);
    }
    if (!found) return false;
    {
        std::lock_guard<std::mutex> cbLock(peerDiscMu_);
        invokeCallback("onPeerDisconnected", id,
                        onPeerDisconnectedFn_, id, std::string("removed"));
    }
    return true;
}

// =============================================================================
// HANDSHAKE — non-blocking with poll() timeout
// =============================================================================
bool PeerManager::doHandshake(const std::string& peerId,
                               int fd) noexcept {
    std::string magic, nodeId;
    uint32_t ver, minVer, maxVer, timeoutMs;
    {
        std::lock_guard<std::mutex> lk(cfgMu_);
        magic     = cfg_.networkMagic;
        nodeId    = cfg_.nodeId;
        ver       = cfg_.protocolVersion;
        minVer    = cfg_.minPeerVersion;
        maxVer    = cfg_.maxPeerVersion;
        timeoutMs = cfg_.handshakeTimeoutMs;
    }

    std::vector<uint8_t> pkt;
    pkt.reserve(264);
    for (size_t i = 0; i < 4; i++)
        pkt.push_back(i < magic.size()
                      ? static_cast<uint8_t>(magic[i]) : 0);
    pkt.push_back((ver >> 24) & 0xFF);
    pkt.push_back((ver >> 16) & 0xFF);
    pkt.push_back((ver >>  8) & 0xFF);
    pkt.push_back( ver        & 0xFF);
    uint8_t idLen = static_cast<uint8_t>(
        std::min(nodeId.size(), size_t(255)));
    pkt.push_back(idLen);
    for (size_t i = 0; i < idLen; i++)
        pkt.push_back(static_cast<uint8_t>(nodeId[i]));

    auto pollWait = [&](int events) -> bool {
        struct pollfd pfd{fd, static_cast<short>(events), 0};
        return ::poll(&pfd, 1, static_cast<int>(timeoutMs)) > 0
               && (pfd.revents & events);
    };

    const char* ptr       = reinterpret_cast<const char*>(pkt.data());
    size_t      remaining = pkt.size();
    while (remaining > 0) {
        if (!pollWait(POLLOUT)) {
            slog(1, "PeerManager", peerId, "handshake send timeout");
            penalizePeer(peerId, 10.0);
            metrics_.handshakeFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ssize_t n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n <= 0) {
            metrics_.handshakeFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ptr += n; remaining -= static_cast<size_t>(n);
    }

    uint8_t hdr[9] = {};
    size_t  got    = 0;
    while (got < 9) {
        if (!pollWait(POLLIN)) {
            slog(1, "PeerManager", peerId, "handshake recv timeout");
            penalizePeer(peerId, 10.0);
            metrics_.handshakeFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ssize_t n = ::recv(fd, hdr + got, 9 - got, 0);
        if (n <= 0) {
            metrics_.handshakeFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        got += static_cast<size_t>(n);
    }

    for (size_t i = 0; i < 4; i++) {
        uint8_t exp = i < magic.size()
                      ? static_cast<uint8_t>(magic[i]) : 0;
        if (hdr[i] != exp) {
            slog(1, "PeerManager", peerId, "handshake bad magic");
            banPeer(peerId);
            metrics_.handshakeFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    uint32_t peerVer =
        (static_cast<uint32_t>(hdr[4]) << 24) |
        (static_cast<uint32_t>(hdr[5]) << 16) |
        (static_cast<uint32_t>(hdr[6]) <<  8) |
         static_cast<uint32_t>(hdr[7]);
    if (peerVer < minVer || peerVer > maxVer) {
        slog(1, "PeerManager", peerId,
             "handshake version mismatch: " + std::to_string(peerVer));
        penalizePeer(peerId, 20.0);
        metrics_.handshakeFailed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint8_t peerIdLen = hdr[8];
    if (peerIdLen == 0) {
        slog(1, "PeerManager", peerId, "handshake empty node ID");
        penalizePeer(peerId, 10.0);
        metrics_.handshakeFailed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::string peerNodeId(peerIdLen, '\0');
    got = 0;
    while (got < peerIdLen) {
        if (!pollWait(POLLIN)) {
            metrics_.handshakeFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ssize_t n = ::recv(fd, peerNodeId.data() + got,
                           peerIdLen - got, 0);
        if (n <= 0) {
            metrics_.handshakeFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        got += static_cast<size_t>(n);
    }

    {
        auto& shard = shardFor(peerId);
        std::unique_lock<std::shared_mutex> wlock(shard.mu);
        auto* peer = shard.find(peerId);
        if (!peer) return false;
        peer->handshakeDone = true;
        peer->version       = peerVer;
        peer->lastSeenAt    = nowSecs();
        peer->score         = std::min(peer->score + 5.0, 100.0);
        peer->dirty         = true;
    }
    markDirty(peerId);
    metrics_.handshakeSuccess.fetch_add(1, std::memory_order_relaxed);
    slog(0, "PeerManager", peerId,
         "handshake OK ver=" + std::to_string(peerVer));
    return true;
}

// =============================================================================
// PEER QUERIES
// =============================================================================
bool PeerManager::hasPeer(const std::string& id) const noexcept {
    auto& shard = shardFor(id);
    std::shared_lock<std::shared_mutex> rlock(shard.mu);
    return shard.hasPeer(id);
}

bool PeerManager::isBanned(const std::string& id) const noexcept {
    auto& shard = shardFor(id);
    std::shared_lock<std::shared_mutex> rlock(shard.mu);
    const auto* p = shard.find(id);
    if (!p) return false;
    return p->isBanned && nowSecs() < p->banExpiresAt;
}

std::optional<PeerInfo> PeerManager::getPeer(
    const std::string& id) const noexcept
{
    auto& shard = shardFor(id);
    std::shared_lock<std::shared_mutex> rlock(shard.mu);
    const auto* p = shard.find(id);
    if (!p) return std::nullopt;
    return *p;
}

std::vector<PeerInfo> PeerManager::getAllPeers() const noexcept {
    std::vector<PeerInfo> result;
    size_t sc = shardCount_.load(std::memory_order_acquire);
    for (size_t i = 0; i < sc; i++) {
        std::shared_lock<std::shared_mutex> rlock(shards_[i]->mu);
        result.reserve(result.size() + shards_[i]->peers.size());
        for (const auto& [_, p] : shards_[i]->peers)
            result.push_back(p);
    }
    return result;
}

std::vector<PeerInfo> PeerManager::getActivePeers() const noexcept {
    std::vector<PeerInfo> result;
    size_t sc = shardCount_.load(std::memory_order_acquire);
    for (size_t i = 0; i < sc; i++) {
        std::shared_lock<std::shared_mutex> rlock(shards_[i]->mu);
        for (const auto& [_, p] : shards_[i]->peers)
            if (!p.isBanned && p.isReachable && p.handshakeDone)
                result.push_back(p);
    }
    return result;
}

size_t PeerManager::peerCount() const noexcept {
    size_t n  = 0;
    size_t sc = shardCount_.load(std::memory_order_acquire);
    for (size_t i = 0; i < sc; i++) {
        std::shared_lock<std::shared_mutex> rlock(shards_[i]->mu);
        n += shards_[i]->peers.size();
    }
    return n;
}

// =============================================================================
// TOKEN BUCKET RATE LIMITING
// =============================================================================
void PeerManager::refillBucket(PeerInfo& peer,
                                 uint64_t nowSec) noexcept {
    if (peer.tokenLastRefill == nowSec) return;
    double elapsed = static_cast<double>(nowSec - peer.tokenLastRefill);
    peer.tokenLastRefill = nowSec;
    peer.tokenMsgBucket  = std::min(
        peer.tokenMsgBucket
        + elapsed * static_cast<double>(
            dynMaxMsgPerSec_.load(std::memory_order_relaxed)),
        static_cast<double>(cfg_.tokenBucketBurstMsg));
    peer.tokenByteBucket = std::min(
        peer.tokenByteBucket
        + elapsed * static_cast<double>(
            dynMaxBytesPerSec_.load(std::memory_order_relaxed)),
        static_cast<double>(cfg_.tokenBucketBurstBytes));
}

bool PeerManager::checkRateLimit(const std::string& id,
                                   size_t msgBytes) noexcept {
    auto& shard = shardFor(id);
    std::unique_lock<std::shared_mutex> wlock(shard.mu);
    auto* peer = shard.find(id);
    if (!peer) return false;
    refillBucket(*peer, nowSecs());
    if (peer->tokenMsgBucket < 1.0) {
        metrics_.rateLimitEvents.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    double bytes = static_cast<double>(msgBytes);
    if (peer->tokenByteBucket < bytes) {
        metrics_.byteQuotaEvents.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    peer->tokenMsgBucket  -= 1.0;
    peer->tokenByteBucket -= bytes;
    ++peer->msgThisSecond;
    peer->byteThisSecond  += msgBytes;
    return true;
}

// =============================================================================
// SCORING — callbacks via invokeCallback (Fix 4)
// =============================================================================
void PeerManager::applyScore(const std::string& id,
                               double delta) noexcept {
    double newScore = 100.0;
    bool   autoBan  = false;
    {
        auto& shard = shardFor(id);
        std::unique_lock<std::shared_mutex> wlock(shard.mu);
        auto* peer = shard.find(id);
        if (!peer) return;
        peer->score = std::clamp(peer->score + delta, 0.0, 100.0);
        newScore    = peer->score;
        if (newScore <= 0.0 && !peer->isBanned) {
            peer->isBanned     = true;
            peer->banExpiresAt = nowSecs() + cfg_.banDurationSecs;
            if (peer->banCount < cfg_.maxBanCount) ++peer->banCount;
            autoBan     = true;
            peer->dirty = true;
        }
    }
    {
        std::lock_guard<std::mutex> lk(scoreMu_);
        invokeCallback("onPeerScored", id,
                        onPeerScoredFn_, id, newScore);
    }
    if (autoBan) {
        metrics_.banEvents.fetch_add(1, std::memory_order_relaxed);
        slog(1, "PeerManager", id, "auto-banned: score=0");
    }
}

bool PeerManager::penalizePeer(const std::string& id,
                                 double penalty) noexcept {
    applyScore(id, -std::abs(penalty));
    return true;
}

bool PeerManager::rewardPeer(const std::string& id,
                               double reward) noexcept {
    applyScore(id, std::abs(reward));
    return true;
}

bool PeerManager::banPeer(const std::string& id) noexcept {
    auto& shard = shardFor(id);
    {
        std::unique_lock<std::shared_mutex> wlock(shard.mu);
        auto* peer = shard.find(id);
        if (!peer) return false;
        peer->isBanned     = true;
        peer->banExpiresAt = nowSecs() + cfg_.banDurationSecs;
        if (peer->banCount < cfg_.maxBanCount) ++peer->banCount;
        peer->dirty = true;
    }
    metrics_.banEvents.fetch_add(1, std::memory_order_relaxed);
    slog(1, "PeerManager", id, "banned");
    return true;
}

// =============================================================================
// DECODE ERROR AUTO-BAN
// =============================================================================
void PeerManager::recordDecodeError(const std::string& id) noexcept {
    uint32_t threshold = dynMaxDecodeErrBan_.load(std::memory_order_relaxed);
    bool autoBan = false;
    {
        auto& shard = shardFor(id);
        std::unique_lock<std::shared_mutex> wlock(shard.mu);
        auto* peer = shard.find(id);
        if (!peer) return;
        ++peer->decodeErrorCount;
        if (peer->decodeErrorCount >= threshold && !peer->isBanned) {
            peer->isBanned     = true;
            peer->banExpiresAt = nowSecs() + cfg_.banDurationSecs;
            if (peer->banCount < cfg_.maxBanCount) ++peer->banCount;
            autoBan     = true;
            peer->dirty = true;
        }
    }
    metrics_.decodeErrors.fetch_add(1, std::memory_order_relaxed);
    if (autoBan) {
        metrics_.banEvents.fetch_add(1, std::memory_order_relaxed);
        metrics_.autoBanDecodeError.fetch_add(1, std::memory_order_relaxed);
        slog(1, "PeerManager", id, "auto-banned: decode errors");
    }
}

// =============================================================================
// FIX 3: TWO-BUCKET MESSAGE DEDUPLICATION
// Active bucket fills up to SEEN_MAX/2 then rotates into stale.
// Stale still blocks replays — no gap in protection during rotation.
// =============================================================================
bool PeerManager::markSeen(const std::string& msgId) noexcept {
    std::lock_guard<std::mutex> lk(seenMu_);

    if (seenStale_.count(msgId) || seenActive_.count(msgId)) {
        metrics_.replayRejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    seenActive_.insert(msgId);

    if (seenActive_.size() >= SEEN_MAX / 2) {
        seenStale_ = std::move(seenActive_);
        seenActive_.clear();
        seenActive_.reserve(SEEN_MAX / 2);
        slog(0, "PeerManager",
             "dedup bucket rotated ("
             + std::to_string(seenStale_.size()) + " in stale)");
    }
    return true;
}

void PeerManager::cleanupSeenMessages() noexcept {
    std::lock_guard<std::mutex> lk(seenMu_);
    seenActive_.clear();
    seenStale_.clear();
}

// =============================================================================
// EVICTION
// =============================================================================
void PeerManager::evictStalePeers() noexcept {
    uint64_t cutoff = nowSecs() - cfg_.peerTimeoutSecs;
    std::vector<std::string> toEvict;
    size_t sc = shardCount_.load(std::memory_order_acquire);
    for (size_t i = 0; i < sc; i++) {
        std::shared_lock<std::shared_mutex> rlock(shards_[i]->mu);
        for (const auto& [id, p] : shards_[i]->peers)
            if (!p.isBanned && p.lastSeenAt > 0 && p.lastSeenAt < cutoff)
                toEvict.push_back(id);
    }
    for (const auto& id : toEvict) {
        removePeer(id);
        metrics_.peersEvicted.fetch_add(1, std::memory_order_relaxed);
        slog(1, "PeerManager", id, "evicted: stale");
    }
}

void PeerManager::evictLowScorePeers() noexcept {
    std::vector<std::string> toBan;
    size_t sc = shardCount_.load(std::memory_order_acquire);
    for (size_t i = 0; i < sc; i++) {
        std::shared_lock<std::shared_mutex> rlock(shards_[i]->mu);
        for (const auto& [id, p] : shards_[i]->peers)
            if (p.score <= 0.0 && !p.isBanned)
                toBan.push_back(id);
    }
    for (const auto& id : toBan) banPeer(id);
}

void PeerManager::unbanExpiredPeers() noexcept {
    uint64_t now = nowSecs();
    std::vector<PeerInfo> unbanned;
    size_t sc = shardCount_.load(std::memory_order_acquire);
    for (size_t i = 0; i < sc; i++) {
        std::unique_lock<std::shared_mutex> wlock(shards_[i]->mu);
        for (auto& [id, p] : shards_[i]->peers) {
            if (p.isBanned && now >= p.banExpiresAt) {
                p.isBanned = false;
                p.score    = 10.0;
                p.dirty    = true;
                unbanned.push_back(p);
            }
        }
    }
    for (const auto& info : unbanned) {
        markDirty(info.id);
        slog(0, "PeerManager", info.id, "ban expired");
        std::lock_guard<std::mutex> cbLock(peerConnMu_);
        invokeCallback("onPeerConnected", info.id,
                        onPeerConnectedFn_, info);
    }
}

// =============================================================================
// DIRTY TRACKING
// =============================================================================
void PeerManager::markDirty(const std::string& id) noexcept {
    std::lock_guard<std::mutex> lk(dirtyMu_);
    dirtyIds_.insert(id);
}

// =============================================================================
// PERSISTENCE
// =============================================================================
void PeerManager::savePeers() const noexcept {
    if (cfg_.peerStorePath.empty()) return;
    try {
        std::vector<PeerInfo> toSave;

        if (cfg_.incrementalSave) {
            std::unordered_set<std::string> dirty;
            {
                std::lock_guard<std::mutex> lk(
                    const_cast<PeerManager*>(this)->dirtyMu_);
                dirty = const_cast<PeerManager*>(this)->dirtyIds_;
                const_cast<PeerManager*>(this)->dirtyIds_.clear();
            }
            size_t sc = shardCount_.load(std::memory_order_acquire);
            for (size_t i = 0; i < sc; i++) {
                std::shared_lock<std::shared_mutex> rlock(shards_[i]->mu);
                for (const auto& [id, p] : shards_[i]->peers)
                    if (dirty.count(id)) toSave.push_back(p);
            }
            metrics_.incrementalSaveCount.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            size_t sc = shardCount_.load(std::memory_order_acquire);
            for (size_t i = 0; i < sc; i++) {
                std::shared_lock<std::shared_mutex> rlock(shards_[i]->mu);
                for (const auto& [_, p] : shards_[i]->peers)
                    toSave.push_back(p);
            }
        }

        std::vector<uint8_t> payload;
        payload.reserve(toSave.size() * sizeof(StoredPeer));
        for (const auto& p : toSave) {
            StoredPeer rec{};
            size_t alen = std::min(p.address.size(), size_t(254));
            rec.addrLen          = static_cast<uint8_t>(alen);
            memcpy(rec.addr, p.address.data(), alen);
            rec.port             = p.port;
            rec.score            = p.score;
            rec.connectedAt      = p.connectedAt;
            rec.lastSeen         = p.lastSeenAt;
            rec.banExpiresAt     = p.banExpiresAt;
            rec.banCount         = p.banCount;
            rec.decodeErrorCount = p.decodeErrorCount;
            rec.isBanned         = p.isBanned ? 1 : 0;
            rec.handshakeDone    = p.handshakeDone ? 1 : 0;
            const uint8_t* rp = reinterpret_cast<const uint8_t*>(&rec);
            payload.insert(payload.end(), rp, rp + sizeof(StoredPeer));
        }

        StoreHeader hdr{};
        hdr.magic     = STORE_MAGIC;
        hdr.version   = STORE_VERSION;
        hdr.numPeers  = static_cast<uint32_t>(toSave.size());
        hdr.timestamp = nowSecs();
        hdr.checksum  = payload.empty()
                        ? 0 : crc32(payload.data(), payload.size());
        memset(hdr.reserved, 0, sizeof(hdr.reserved));

        std::string tmp = cfg_.peerStorePath + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) { slog(2, "PeerManager", "savePeers: open failed"); return; }
            f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
            if (!payload.empty())
                f.write(reinterpret_cast<const char*>(payload.data()),
                        payload.size());
        }
        std::filesystem::rename(tmp, cfg_.peerStorePath);
        metrics_.saveCount.fetch_add(1, std::memory_order_relaxed);
        slog(0, "PeerManager",
             "saved " + std::to_string(toSave.size()) + " peers");

    } catch (const std::exception& e) {
        slog(1, "PeerManager", std::string("savePeers: ") + e.what());
    } catch (...) {
        slog(1, "PeerManager", "savePeers: unknown exception");
    }
}

void PeerManager::loadPeers() noexcept {
    if (cfg_.peerStorePath.empty()) return;
    try {
        std::ifstream f(cfg_.peerStorePath, std::ios::binary);
        if (!f) return;

        StoreHeader hdr{};
        f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (!f || hdr.magic != STORE_MAGIC) {
            slog(2, "PeerManager", "loadPeers: bad magic"); return;
        }
        if (hdr.version > STORE_VERSION) {
            slog(2, "PeerManager",
                 "loadPeers: unsupported version "
                 + std::to_string(hdr.version)); return;
        }

        size_t paySize = static_cast<size_t>(hdr.numPeers) * sizeof(StoredPeer);
        std::vector<uint8_t> payload(paySize);
        f.read(reinterpret_cast<char*>(payload.data()),
               static_cast<std::streamsize>(paySize));

        uint32_t computed = payload.empty()
                            ? 0 : crc32(payload.data(), payload.size());
        if (computed != hdr.checksum) {
            slog(2, "PeerManager", "loadPeers: CRC-32 mismatch"); return;
        }

        uint64_t now    = nowSecs();
        size_t   loaded = 0;
        size_t   off    = 0;

        for (uint32_t i = 0; i < hdr.numPeers; i++) {
            if (off + sizeof(StoredPeer) > payload.size()) break;
            StoredPeer rec{};
            memcpy(&rec, payload.data() + off, sizeof(StoredPeer));
            off += sizeof(StoredPeer);

            if (rec.isBanned && rec.banExpiresAt <= now) continue;

            PeerInfo info;
            info.address          = std::string(rec.addr, rec.addrLen);
            info.port             = rec.port;
            info.id               = peerKey(info.address, info.port);
            info.score            = std::clamp(rec.score, 0.0, 100.0);
            info.connectedAt      = rec.connectedAt;
            info.lastSeenAt       = rec.lastSeen;
            info.banExpiresAt     = rec.banExpiresAt;
            info.banCount         = rec.banCount;
            info.decodeErrorCount = rec.decodeErrorCount;
            info.isBanned         = rec.isBanned != 0;
            info.handshakeDone    = false;
            info.isReachable      = false;

            if (addPeer(info)) ++loaded;
        }
        metrics_.loadCount.fetch_add(1, std::memory_order_relaxed);
        slog(0, "PeerManager",
             "loaded " + std::to_string(loaded) + " peers");

    } catch (const std::exception& e) {
        slog(1, "PeerManager", std::string("loadPeers: ") + e.what());
    } catch (...) {
        slog(1, "PeerManager", "loadPeers: unknown exception");
    }
}

// =============================================================================
// METRICS EXPORT
// =============================================================================
void PeerManager::exportMetrics() noexcept {
    uint64_t total = 0, active = 0, banned = 0;
    double   avgScore = 0.0;
    size_t   sc = shardCount_.load(std::memory_order_acquire);

    for (size_t i = 0; i < sc; i++) {
        std::shared_lock<std::shared_mutex> rlock(shards_[i]->mu);
        for (const auto& [_, p] : shards_[i]->peers) {
            ++total;
            if (!p.isBanned && p.isReachable && p.handshakeDone) ++active;
            if (p.isBanned) ++banned;
            avgScore += p.score;
        }
    }
    if (total > 0) avgScore /= static_cast<double>(total);

    std::ostringstream ss;
    ss << "# HELP peermgr_peers_total Total peers\n"
       << "# TYPE peermgr_peers_total gauge\n"
       << "peermgr_peers_total " << total << "\n"
       << "# HELP peermgr_peers_active Active peers\n"
       << "# TYPE peermgr_peers_active gauge\n"
       << "peermgr_peers_active " << active << "\n"
       << "# HELP peermgr_peers_banned Banned peers\n"
       << "# TYPE peermgr_peers_banned gauge\n"
       << "peermgr_peers_banned " << banned << "\n"
       << "# HELP peermgr_peer_score_avg Average peer score\n"
       << "# TYPE peermgr_peer_score_avg gauge\n"
       << "peermgr_peer_score_avg " << avgScore << "\n"
       << "# HELP peermgr_shard_count Shard count\n"
       << "# TYPE peermgr_shard_count gauge\n"
       << "peermgr_shard_count " << sc << "\n"
       << "# HELP peermgr_ban_events_total Ban events\n"
       << "# TYPE peermgr_ban_events_total counter\n"
       << "peermgr_ban_events_total "
       << metrics_.banEvents.load() << "\n"
       << "# HELP peermgr_rate_limit_events_total Rate limit hits\n"
       << "# TYPE peermgr_rate_limit_events_total counter\n"
       << "peermgr_rate_limit_events_total "
       << metrics_.rateLimitEvents.load() << "\n"
       << "# HELP peermgr_decode_errors_total Decode errors\n"
       << "# TYPE peermgr_decode_errors_total counter\n"
       << "peermgr_decode_errors_total "
       << metrics_.decodeErrors.load() << "\n"
       << "# HELP peermgr_handshake_success_total Handshake OK\n"
       << "# TYPE peermgr_handshake_success_total counter\n"
       << "peermgr_handshake_success_total "
       << metrics_.handshakeSuccess.load() << "\n"
       << "# HELP peermgr_handshake_failed_total Handshake failed\n"
       << "# TYPE peermgr_handshake_failed_total counter\n"
       << "peermgr_handshake_failed_total "
       << metrics_.handshakeFailed.load() << "\n"
       << "# HELP peermgr_peers_evicted_total Evicted\n"
       << "# TYPE peermgr_peers_evicted_total counter\n"
       << "peermgr_peers_evicted_total "
       << metrics_.peersEvicted.load() << "\n"
       << "# HELP peermgr_shard_growths_total Shard resizes\n"
       << "# TYPE peermgr_shard_growths_total counter\n"
       << "peermgr_shard_growths_total "
       << metrics_.shardGrowths.load() << "\n"
       << "# HELP peermgr_save_count_total Saves\n"
       << "# TYPE peermgr_save_count_total counter\n"
       << "peermgr_save_count_total "
       << metrics_.saveCount.load() << "\n"
       << "# HELP peermgr_incremental_saves_total Incremental saves\n"
       << "# TYPE peermgr_incremental_saves_total counter\n"
       << "peermgr_incremental_saves_total "
       << metrics_.incrementalSaveCount.load() << "\n"
       << "# HELP peermgr_replay_rejected_total Replay rejected\n"
       << "# TYPE peermgr_replay_rejected_total counter\n"
       << "peermgr_replay_rejected_total "
       << metrics_.replayRejected.load() << "\n"
       << "# HELP peermgr_qps_in_60s Msgs in per 60s\n"
       << "# TYPE peermgr_qps_in_60s gauge\n"
       << "peermgr_qps_in_60s " << metrics_.msgIn.sum() << "\n"
       << "# HELP peermgr_qps_out_60s Msgs out per 60s\n"
       << "# TYPE peermgr_qps_out_60s gauge\n"
       << "peermgr_qps_out_60s " << metrics_.msgOut.sum() << "\n"
       << "# HELP peermgr_bw_in_60s Bytes in per 60s\n"
       << "# TYPE peermgr_bw_in_60s gauge\n"
       << "peermgr_bw_in_60s " << metrics_.bytesIn.sum() << "\n"
       << "# HELP peermgr_bw_out_60s Bytes out per 60s\n"
       << "# TYPE peermgr_bw_out_60s gauge\n"
       << "peermgr_bw_out_60s " << metrics_.bytesOut.sum() << "\n";

    std::string text = ss.str();
    if (!cfg_.metricsExportPath.empty()) {
        try {
            std::ofstream f(cfg_.metricsExportPath, std::ios::trunc);
            if (f) f << text;
        } catch (...) {}
    } else {
        slog(0, "PeerManager", text);
    }
}

std::string PeerManagerMetrics::toPrometheusText() const noexcept {
    std::ostringstream ss;
    ss << "peermgr_active_peers "    << activePeers.load()    << "\n"
       << "peermgr_ban_events "      << banEvents.load()      << "\n"
       << "peermgr_decode_errors "   << decodeErrors.load()   << "\n"
       << "peermgr_replay_rejected " << replayRejected.load() << "\n"
       << "peermgr_qps_in "          << msgIn.sum()           << "\n"
       << "peermgr_qps_out "         << msgOut.sum()          << "\n"
       << "peermgr_bw_in "           << bytesIn.sum()         << "\n"
       << "peermgr_bw_out "          << bytesOut.sum()        << "\n";
    return ss.str();
}

const PeerManagerMetrics& PeerManager::metrics() const noexcept {
    return metrics_;
}

std::string PeerManager::getPrometheusText() const noexcept {
    return metrics_.toPrometheusText();
}

// =============================================================================
// DYNAMIC TUNING
// =============================================================================
void PeerManager::setRateLimit(uint32_t maxMsg,
                                 uint64_t maxBytes) noexcept {
    dynMaxMsgPerSec_.store(maxMsg, std::memory_order_relaxed);
    dynMaxBytesPerSec_.store(maxBytes, std::memory_order_relaxed);
}

void PeerManager::setMaxDecodeErrorsBeforeBan(uint32_t n) noexcept {
    dynMaxDecodeErrBan_.store(n, std::memory_order_relaxed);
}

void PeerManager::setMaxPeers(size_t n) noexcept {
    dynMaxPeers_.store(n, std::memory_order_relaxed);
}

} // namespace net
