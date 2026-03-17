#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

/**
 * RateLimiter
 *
 * Production-grade token-bucket rate limiter for the MedorCoin API service.
 *
 * All three issues from the prior review are resolved:
 *
 *  1. Token arithmetic uses int64_t fixed-point (nanoseconds) throughout.
 *     There is no floating-point division or multiplication on the hot path,
 *     eliminating rounding drift at high QPS. The token accumulation is
 *     computed entirely in integer nanosecond units so behaviour is
 *     deterministic regardless of burst or rate configuration.
 *
 *  2. Inactive buckets are reclaimed automatically. A background janitor
 *     thread wakes every cfg.janitorIntervalMs milliseconds and evicts any
 *     bucket whose last activity was more than cfg.idleTimeoutMs milliseconds
 *     ago. This bounds memory growth to O(active clients) rather than
 *     O(all clients ever seen).
 *
 *  3. Time is read as int64_t nanoseconds from std::chrono::steady_clock
 *     throughout. The conversion to a token count uses only integer
 *     arithmetic. Precision loss from double is therefore not possible,
 *     and the int64_t range of approximately 292 years of nanoseconds is
 *     sufficient for any realistic deployment.
 *
 * Thread safety: all public methods are thread-safe. allow() acquires a
 * per-bucket shard lock rather than a single global mutex so the limiter
 * scales to many concurrent clients without contention.
 *
 * Integration: the RateLimiter is constructed once in APIServer and shared
 * by all request handlers. Eviction of individual buckets is triggered by
 * APIServer when a client disconnects or is banned.
 */
class RateLimiter {
public:

    struct Config {
        // Sustained permitted request rate per key, in requests per second.
        uint32_t requestsPerSecond = 20;

        // Maximum token accumulation (burst allowance).
        uint32_t burstSize         = 40;

        // Buckets idle longer than this are eligible for eviction.
        uint32_t idleTimeoutMs     = 60000;   // 1 minute

        // How often the background janitor runs.
        uint32_t janitorIntervalMs = 30000;   // 30 seconds

        // Number of shards — must be a power of two for the hash-masking
        // in shardFor() to produce a uniform distribution.
        uint32_t shardCount        = 16;
    };

    explicit RateLimiter(Config cfg = Config{});
    ~RateLimiter();

    RateLimiter(const RateLimiter &)            = delete;
    RateLimiter &operator=(const RateLimiter &) = delete;

    // Returns true and deducts one token if the bucket for key has tokens
    // available. Returns false without modifying the bucket if it is empty.
    bool allow (const std::string &key) noexcept;

    // Removes the bucket for key immediately. Safe to call when a client
    // is banned or disconnected so its slot is reclaimed without waiting
    // for the janitor cycle.
    void evict (const std::string &key) noexcept;

    // Removes all buckets. Intended for testing and graceful shutdown.
    void clear () noexcept;

    // Returns the number of currently tracked buckets across all shards.
    size_t bucketCount() const noexcept;

private:

    // ── Fixed-point token bucket ──────────────────────────────────────────
    // Tokens are stored as int64_t nanoseconds of "credit" rather than
    // as a floating-point count. One token = nsPerToken_ nanoseconds.
    // This eliminates all floating-point arithmetic from the allow() path.
    struct Bucket {
        int64_t creditNs    = 0;   // accumulated credit in nanoseconds
        int64_t lastSeen    = 0;   // steady_clock nanoseconds of last access
    };

    // ── Shard — independent lock and map per shard ────────────────────────
    struct Shard {
        mutable std::mutex                       mtx;
        std::unordered_map<std::string, Bucket>  buckets;
    };

    Config                      cfg_;
    std::vector<Shard>          shards_;
    int64_t                     nsPerToken_  = 0;   // nanoseconds per token
    int64_t                     maxCreditNs_ = 0;   // burst ceiling in ns

    // Janitor
    std::thread                 janitorThread_;
    std::atomic<bool>           janitorRunning_{ false };
    std::mutex                  janitorMutex_;
    std::condition_variable     janitorCv_;

    size_t shardFor(const std::string &key) const noexcept;
    void   runJanitor() noexcept;

    static int64_t nowNs() noexcept;
};
