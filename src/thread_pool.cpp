#include "thread_pool.h"

ThreadPool::ThreadPool(size_t workerCount, size_t maxQueueDepth)
    : maxDepth_(maxQueueDepth)
{
    workers_.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i)
        workers_.emplace_back([this]() { workerLoop(); });
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto &t : workers_)
        if (t.joinable()) t.join();
}

void ThreadPool::workerLoop() noexcept
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop();
        }
        try { task(); } catch (...) {}
    }
}

size_t ThreadPool::pendingTasks() const noexcept
{
    std::unique_lock<std::mutex> lock(queueMutex_);
    return queue_.size();
}

void ThreadPool::drain() noexcept
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (queue_.empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
