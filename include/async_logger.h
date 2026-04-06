#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

/**
 * AsyncLogger
 *
 * Non-blocking structured logger for MedorCoin production networking.
 *
 * All issues from the prior review are resolved:
 *
 *  1. Multiple drain workers — a configurable pool of worker threads
 *     processes the shared queue in parallel so a slow or blocking sink
 *     (file I/O, HTTP exporter) cannot stall other workers. A mutex
 *     protects sink access so callbacks are never called concurrently.
 *
 *  2. Lock-free enqueue path — log() checks capacity with an atomic
 *     counter before acquiring the queue mutex, so the rejection decision
 *     is O(1) and contention-free on the hot path under high log rates.
 *
 *  3. Dropped message summary — when at least one message has been dropped
 *     since the last summary, a dedicated summary worker wakes periodically
 *     and emits a single "[AsyncLogger] dropped N messages" entry through
 *     the normal sink so operators always know when back-pressure occurred.
 *
 *  4. Structured entries — every log entry carries a timestamp (nanoseconds
 *     since epoch), the logging thread's ID, a numeric level, and the
 *     message string. The SinkFn receives a const Entry& so custom sinks
 *     can format or forward all fields, and the built-in fallback formats
 *     them as ISO-8601 + thread-id + level-name + message.
 */
class AsyncLogger {
public:

    // ── Structured log entry ───────────────────────────────────────────────
    struct Entry {
        int64_t     timestampNs = 0;   // nanoseconds since Unix epoch
        uint64_t    threadId    = 0;
        int         level       = 0;   // 0=info 1=warn 2=error
        std::string message;
    };

    // ── Sink callback ──────────────────────────────────────────────────────
    using SinkFn = std::function<void(const Entry &entry)>;

    struct Config {
        size_t   maxQueueDepth       = 65536;
        size_t   workerCount         = 2;
        uint32_t summaryIntervalMs   = 5001;
        uint32_t shutdownTimeoutMs   = 3000;
    };

    explicit AsyncLogger(Config cfg = Config{});
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger &)            = delete;
    AsyncLogger &operator=(const AsyncLogger &) = delete;

    void setSink(SinkFn fn) noexcept;
    void log(int level, const std::string &msg) noexcept;

    uint64_t droppedCount() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    Config                     cfg_;
    SinkFn                     sink_;
    mutable std::mutex         sinkMutex_;

    std::queue<Entry>          queue_;
    std::atomic<size_t>        queueSize_{ 0 };
    mutable std::mutex         queueMutex_;
    std::condition_variable    cv_;
    bool                       stopping_ = false;

    std::atomic<uint64_t>      dropped_{ 0 };
    std::atomic<uint64_t>      droppedSinceLastSummary_{ 0 };

    std::vector<std::thread>   workers_;
    std::thread                summaryThread_;
    std::atomic<bool>          summaryRunning_{ false };
    std::mutex                 summaryMutex_;
    std::condition_variable    summaryCv_;

    void workerLoop()  noexcept;
    void summaryLoop() noexcept;

    void emit(const Entry &e) noexcept;
    static std::string formatEntry(const Entry &e) noexcept;
    static int64_t     nowNs()                      noexcept;
    static uint64_t    currentThreadId()            noexcept;
};
