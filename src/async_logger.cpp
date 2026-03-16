#include "async_logger.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#if defined(__linux__) || defined(__APPLE__)
#  include <unistd.h>
#  include <sys/syscall.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Static helpers
// ─────────────────────────────────────────────────────────────────────────────

int64_t AsyncLogger::nowNs() noexcept
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t AsyncLogger::currentThreadId() noexcept
{
    // std::this_thread::get_id() is not directly printable as an integer on
    // all platforms. We hash it to a uint64 for structured log fields.
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

std::string AsyncLogger::formatEntry(const Entry &e) noexcept
{
    // Format: "YYYY-MM-DDTHH:MM:SS.nnnnnnnnn [tid] [LEVEL] message"
    const int64_t secs   = e.timestampNs / 1'000'000'000LL;
    const int64_t nanos  = e.timestampNs % 1'000'000'000LL;
    const std::time_t tt = static_cast<std::time_t>(secs);

    std::tm tmBuf{};
#if defined(_WIN32)
    gmtime_s(&tmBuf, &tt);
#else
    gmtime_r(&tt, &tmBuf);
#endif

    std::ostringstream ss;
    ss << std::put_time(&tmBuf, "%Y-%m-%dT%H:%M:%S")
       << "." << std::setw(9) << std::setfill('0') << nanos
       << " [" << std::hex << std::setw(16) << std::setfill('0')
       << e.threadId << std::dec << "]";

    if      (e.level >= 2) ss << " [ERROR] ";
    else if (e.level == 1) ss << " [WARN]  ";
    else                   ss << " [INFO]  ";

    ss << e.message;
    return ss.str();
}

void AsyncLogger::emit(const Entry &e) noexcept
{
    std::lock_guard<std::mutex> lock(sinkMutex_);
    if (sink_) {
        try { sink_(e); } catch (...) {}
    } else {
        const std::string line = formatEntry(e);
        if (e.level >= 2)
            std::cerr << line << "\n";
        else
            std::cout << line << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

AsyncLogger::AsyncLogger(Config cfg)
    : cfg_(std::move(cfg))
{
    workers_.reserve(cfg_.workerCount);
    for (size_t i = 0; i < cfg_.workerCount; ++i)
        workers_.emplace_back([this]() { workerLoop(); });

    summaryRunning_.store(true);
    summaryThread_ = std::thread([this]() { summaryLoop(); });
}

AsyncLogger::~AsyncLogger()
{
    // Signal all workers
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        stopping_ = true;
    }
    cv_.notify_all();

    // Stop summary thread
    summaryRunning_.store(false);
    summaryCv_.notify_all();

    const auto deadline =
        std::chrono::steady_clock::now()
        + std::chrono::milliseconds(cfg_.shutdownTimeoutMs);

    for (auto &t : workers_) {
        if (!t.joinable()) continue;
        // Timed join via helper future
        auto p = std::make_shared<std::promise<void>>();
        auto f = p->get_future();
        std::thread helper([th = std::move(t), pp = p]() mutable {
            th.join(); pp->set_value();
        });
        helper.detach();
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining > std::chrono::milliseconds(0))
            f.wait_for(remaining);
    }

    if (summaryThread_.joinable()) {
        auto p = std::make_shared<std::promise<void>>();
        auto f = p->get_future();
        std::thread h([t = std::move(summaryThread_), pp = p]() mutable {
            t.join(); pp->set_value();
        });
        h.detach();
        f.wait_for(std::chrono::milliseconds(cfg_.shutdownTimeoutMs));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setSink
// ─────────────────────────────────────────────────────────────────────────────

void AsyncLogger::setSink(SinkFn fn) noexcept
{
    std::lock_guard<std::mutex> lock(sinkMutex_);
    sink_ = std::move(fn);
}

// ─────────────────────────────────────────────────────────────────────────────
// log — lock-free capacity check on the hot path
// ─────────────────────────────────────────────────────────────────────────────

void AsyncLogger::log(int level, const std::string &msg) noexcept
{
    // Reject immediately without acquiring the queue mutex when full.
    // This is the critical hot path — zero lock contention on rejection.
    if (queueSize_.load(std::memory_order_relaxed) >= cfg_.maxQueueDepth) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        droppedSinceLastSummary_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Entry e;
    e.timestampNs = nowNs();
    e.threadId    = currentThreadId();
    e.level       = level;
    e.message     = msg;

    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        if (stopping_) return;

        // Re-check under lock to prevent TOCTOU
        if (queueSize_.load(std::memory_order_relaxed) >= cfg_.maxQueueDepth) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            droppedSinceLastSummary_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        queue_.push(std::move(e));
        queueSize_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
}

// ─────────────────────────────────────────────────────────────────────────────
// workerLoop — each worker independently dequeues and emits entries
// The sink mutex inside emit() ensures the sink is never called concurrently.
// ─────────────────────────────────────────────────────────────────────────────

void AsyncLogger::workerLoop() noexcept
{
    for (;;) {
        Entry e;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            cv_.wait(lock, [this]() {
                return stopping_ || !queue_.empty();
            });
            if (stopping_ && queue_.empty()) return;
            e = std::move(queue_.front());
            queue_.pop();
            queueSize_.fetch_sub(1, std::memory_order_relaxed);
        }
        emit(e);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// summaryLoop — periodically emits a summary of dropped messages so operators
//               always know when back-pressure has occurred, even if the
//               dropped entries themselves could not be logged.
// ─────────────────────────────────────────────────────────────────────────────

void AsyncLogger::summaryLoop() noexcept
{
    while (summaryRunning_.load()) {
        {
            std::unique_lock<std::mutex> lock(summaryMutex_);
            summaryCv_.wait_for(lock,
                std::chrono::milliseconds(cfg_.summaryIntervalMs),
                [this]() { return !summaryRunning_.load(); });
        }
        if (!summaryRunning_.load()) return;

        const uint64_t n = droppedSinceLastSummary_.exchange(
            0, std::memory_order_relaxed);

        if (n > 0) {
            Entry summary;
            summary.timestampNs = nowNs();
            summary.threadId    = currentThreadId();
            summary.level       = 1;   // warn
            summary.message     = "[AsyncLogger] " + std::to_string(n)
                                + " log message(s) dropped due to queue "
                                  "back-pressure since last summary";
            emit(summary);
        }
    }
}
