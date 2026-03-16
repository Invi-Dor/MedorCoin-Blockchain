#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

/**
 * ThreadPool
 *
 * Fixed-size worker thread pool with bounded task queue.
 * Replaces detached-thread-per-connection with a controlled concurrency model.
 * submit() returns a std::future so callers can optionally wait for completion.
 * When the task queue is full, submit() returns an invalid future rather than
 * blocking or throwing, allowing the caller to apply back-pressure.
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t workerCount, size_t maxQueueDepth = 4096);
    ~ThreadPool();

    ThreadPool(const ThreadPool &)            = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // Enqueues a task. Returns a valid future on success, an invalid
    // future if the queue is full (caller should apply back-pressure).
    template<typename F, typename... Args>
    std::future<void> submit(F &&f, Args &&...args) noexcept
    {
        auto task = std::make_shared<std::packaged_task<void()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<void> fut = task->get_future();

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (stopping_ || queue_.size() >= maxDepth_)
                return {};
            queue_.push([task]() { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    size_t pendingTasks() const noexcept;
    void   drain()              noexcept;

private:
    size_t                          maxDepth_;
    std::vector<std::thread>        workers_;
    std::queue<std::function<void()>> queue_;
    mutable std::mutex              queueMutex_;
    std::condition_variable         cv_;
    bool                            stopping_ = false;

    void workerLoop() noexcept;
};
