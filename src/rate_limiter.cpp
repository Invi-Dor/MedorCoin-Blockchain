#include "rate_limiter.h"

#include <algorithm>
#include <cassert>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

RateLimiter::RateLimiter(Config cfg)
    : cfg_(std::move(cfg))
{
    // Enforce that shardCount is at least 1 and a power of two.
    if (cfg_.shardCount == 0 || (cfg_.shardCount & (cfg_.shardCount - 1)) != 0) {
        std::cerr << "[RateLimiter] shardCount must be a power of two; "
                     "defaulting to 16\n";
        cfg_.shardCount = 16;
    }

    if (cfg_.requestsPerSecond == 0) {
        std::cerr << "[RateLimiter] requestsPerSecond must not be zero; "
                     "defaulting to 1\n";
        cfg_.requestsPerSecond = 1;
    }

    if (cfg_.burstSize < cfg_.requestsPerSecond)
        cfg_.burstSize = cfg_.requestsPerSecond;

    // Compute fixed-point constants.
    // nsPerToken_ is the number of nanoseconds that must elapse to earn
    // one token. A rate of R req/s means one token every (1e9 / R) ns.
    nsPerToken_  = static_cast<int64_t>(1'000'000'000LL / cfg_.requestsPerSecond);
    maxCreditNs_ = static_cast<int64_t>(cfg_.burstSize) * nsPerToken_;

    shards_.resize(cfg_.shardCount);

    // Start the background janitor
    janitorRunning_.store(true);
    janitorThread_ = std::thread([this]() { runJanitor(); });
}

RateLimiter::~RateLimiter()
{
    janitorRunning_.store(false);
    janitorCv_.notify_all();
    if (janitorThread_.joinable())
        janitorThread_.join();
}

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

int64_t RateLimiter::nowNs() noexcept
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

size_t RateLimiter::shardFor(const std::string &key) const noexcept
{
    // Mask with (shardCount - 1) because shardCount is guaranteed a power of 2.
    return std::hash<std::string>{}(key) & (cfg_.shardCount - 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// allow
//
// Token accumulation uses only int64_t arithmetic.
//
// The "credit" model: each bucket holds a credit balance in nanoseconds.
// One request costs nsPerToken_ nanoseconds of credit. Credit accrues at
// one nsPerToken_ per nsPerToken_ of elapsed wall time, capped at
// maxCreditNs_ (the burst ceiling).
//
// When a request arrives:
//   elapsed = now - bucket.lastSeen
//   bucket.creditNs = min(maxCreditNs_, bucket.creditNs + elapsed)
//   if bucket.creditNs >= nsPerToken_: deduct and permit
//   else: deny
//
// This is arithmetically identical to a token bucket with floating-point
// tokens but avoids all floating-point operations.
// ─────────────────────────────────────────────────────────────────────────────

bool RateLimiter::allow(const std::string &key) noexcept
{
    if (key.empty()) return false;

    const int64_t now = nowNs();
    Shard &shard = shards_[shardFor(key)];

    std::lock_guard<std::mutex> lock(shard.mtx);

    auto &bucket = shard.buckets[key];

    if (bucket.lastSeen == 0) {
        // New bucket — start with a full burst allowance so the first
        // cfg_.burstSize requests are permitted immediately.
        bucket.creditNs = maxCreditNs_;
        bucket.lastSeen = now;
    } else {
        const int64_t elapsed = now - bucket.lastSeen;

        // Guard against backward clock jumps (e.g. NTP correction)
        if (elapsed > 0) {
            bucket.creditNs += elapsed;
            if (bucket.creditNs > maxCreditNs_)
                bucket.creditNs = maxCreditNs_;
        }
        bucket.lastSeen = now;
    }

    if (bucket.creditNs < nsPerToken_)
        return false;

    bucket.creditNs -= nsPerToken_;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// evict — removes one bucket from whichever shard owns it
// ─────────────────────────────────────────────────────────────────────────────

void RateLimiter::evict(const std::string &key) noexcept
{
    if (key.empty()) return;
    Shard &shard = shards_[shardFor(key)];
    std::lock_guard<std::mutex> lock(shard.mtx);
    shard.buckets.erase(key);
}

// ─────────────────────────────────────────────────────────────────────────────
// clear
// ─────────────────────────────────────────────────────────────────────────────

void RateLimiter::clear() noexcept
{
    for (auto &shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mtx);
        shard.buckets.clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// bucketCount
// ─────────────────────────────────────────────────────────────────────────────

size_t RateLimiter::bucketCount() const noexcept
{
    size_t total = 0;
    for (const auto &shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mtx);
        total += shard.buckets.size();
    }
    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// runJanitor
//
// Wakes every cfg_.janitorIntervalMs and evicts any bucket whose lastSeen
// timestamp is older than cfg_.idleTimeoutMs. Each shard is locked
// independently so only one shard is blocked at a time; active shards
// continue serving requests while other shards are being cleaned.
// ─────────────────────────────────────────────────────────────────────────────

void RateLimiter::runJanitor() noexcept
{
    while (janitorRunning_.load()) {
        {
            std::unique_lock<std::mutex> lock(janitorMutex_);
            janitorCv_.wait_for(lock,
                std::chrono::milliseconds(cfg_.janitorIntervalMs),
                [this]() { return !janitorRunning_.load(); });
        }
        if (!janitorRunning_.load()) return;

        const int64_t cutoffNs = nowNs()
            - static_cast<int64_t>(cfg_.idleTimeoutMs) * 1'000'000LL;

        size_t evicted = 0;

        for (auto &shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mtx);
            for (auto it = shard.buckets.begin();
                 it != shard.buckets.end(); )
            {
                if (it->second.lastSeen < cutoffNs) {
                    it = shard.buckets.erase(it);
                    ++evicted;
                } else {
                    ++it;
                }
            }
        }

        if (evicted > 0)
            std::cout << "[RateLimiter] janitor evicted "
                      << evicted << " idle bucket(s); "
                      << bucketCount() << " remaining\n";
    }
}
