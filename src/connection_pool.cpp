// =============================================================================
// ConnectionPool.cpp
// MedorCoin P2P — Production-grade TCP connection pool implementation
// Covers: Linux (epoll), macOS/BSD (kqueue), POSIX fallback (poll)
// =============================================================================

#include "ConnectionPool.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#  include <sys/epoll.h>
#  define POOL_USE_EPOLL  1
#elif defined(__APPLE__) || defined(__FreeBSD__) || \
      defined(__OpenBSD__) || defined(__NetBSD__)
#  include <sys/event.h>
#  define POOL_USE_KQUEUE 1
#else
#  include <poll.h>
#  define POOL_USE_POLL   1
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <stdexcept>

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

uint64_t monotonicMs() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count());
}

uint64_t monotonicSecs() noexcept {
    return monotonicMs() / 1000u;
}

bool setNonBlocking(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool setBlocking(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

// Returns the resolved addrinfo list; caller must freeaddrinfo().
// Returns nullptr on failure.
addrinfo* resolveHost(const std::string& host, uint16_t port) noexcept {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_ADDRCONFIG;

    const std::string svc = std::to_string(port);
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), svc.c_str(), &hints, &res) != 0)
        return nullptr;
    return res;
}

} // anonymous namespace

// =============================================================================
// Static helpers
// =============================================================================

/*static*/ std::string
ConnectionPool::makeKey(const std::string& h, uint16_t p) noexcept {
    return h + ':' + std::to_string(p);
}

/*static*/ uint64_t ConnectionPool::nowSecs() noexcept { return monotonicSecs(); }
/*static*/ uint64_t ConnectionPool::nowMs()   noexcept { return monotonicMs();   }

/*static*/ void
ConnectionPool::joinWithTimeout(std::thread& t, uint32_t timeoutMs) noexcept {
    if (!t.joinable()) return;
    // detach after timeout to avoid blocking forever
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeoutMs);
    // std::thread has no timed-join; we spin-sleep briefly
    while (t.joinable()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            t.detach();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    t.join();
}

// =============================================================================
// applySocketOptions
// Platform-normalised keepalive + nodelay
// =============================================================================

/*static*/ void ConnectionPool::applySocketOptions(int fd) noexcept {
    // TCP_NODELAY — disable Nagle
    {
        int v = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
    }

    // SO_KEEPALIVE — enable kernel keepalive
    {
        int v = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof(v));
    }

    // Idle time before first probe: 60s
#if defined(TCP_KEEPIDLE)
    { int v = 60; ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,    &v, sizeof(v)); }
#elif defined(TCP_KEEPALIVE)   // macOS uses TCP_KEEPALIVE for idle
    { int v = 60; ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE,   &v, sizeof(v)); }
#endif

    // Probe interval: 10s
#if defined(TCP_KEEPINTVL)
    { int v = 10; ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL,   &v, sizeof(v)); }
#endif

    // Number of probes: 3
#if defined(TCP_KEEPCNT)
    { int v = 3;  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,     &v, sizeof(v)); }
#endif

    // SO_LINGER off — RST on close, don't linger
    {
        struct linger l{ 0, 0 };
        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    }
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

ConnectionPool::ConnectionPool(Config cfg)
    : cfg_(std::move(cfg))
{
    // ── Reactor ───────────────────────────────────────────────────────────
    if (!initReactor())
        throw std::runtime_error("ConnectionPool: failed to initialise reactor");

    reactorRunning_.store(true);
    reactorThread_ = std::thread(&ConnectionPool::runReactor, this);

    // ── DNS workers ───────────────────────────────────────────────────────
    dnsRunning_.store(true);
    for (size_t i = 0; i < cfg_.dnsWorkerCount; ++i)
        dnsWorkers_.emplace_back(&ConnectionPool::runDnsWorker, this);

    // ── TLS workers ───────────────────────────────────────────────────────
    tlsRunning_.store(true);
    for (size_t i = 0; i < cfg_.tlsWorkerCount; ++i)
        tlsWorkers_.emplace_back(&ConnectionPool::runTlsWorker, this);

    // ── Async acquire workers ─────────────────────────────────────────────
    asyncRunning_.store(true);
    for (size_t i = 0; i < cfg_.asyncWorkerCount; ++i)
        asyncWorkers_.emplace_back(&ConnectionPool::runAsyncWorker, this);

    // ── Janitor workers ───────────────────────────────────────────────────
    janitorRunning_.store(true);
    for (size_t i = 0; i < cfg_.janitorWorkerCount; ++i)
        janitorWorkers_.emplace_back(&ConnectionPool::runJanitorWorker, this, i);

    // ── Metrics reporter ──────────────────────────────────────────────────
    metricsRunning_.store(true);
    metricsThread_ = std::thread(&ConnectionPool::runMetricsReporter, this);
}

ConnectionPool::~ConnectionPool() {
    // ── Signal all workers to stop ────────────────────────────────────────
    reactorRunning_.store(false);
    dnsRunning_    .store(false);
    tlsRunning_    .store(false);
    asyncRunning_  .store(false);
    janitorRunning_.store(false);
    metricsRunning_.store(false);

    dnsCv_     .notify_all();
    tlsCv_     .notify_all();
    asyncCv_   .notify_all();
    janitorCv_ .notify_all();
    metricsCv_ .notify_all();

    // ── Join all threads with bounded timeout ─────────────────────────────
    joinWithTimeout(reactorThread_,  cfg_.shutdownTimeoutMs);
    joinWithTimeout(metricsThread_,  cfg_.shutdownTimeoutMs);

    for (auto& t : dnsWorkers_)     joinWithTimeout(t, cfg_.shutdownTimeoutMs);
    for (auto& t : tlsWorkers_)     joinWithTimeout(t, cfg_.shutdownTimeoutMs);
    for (auto& t : asyncWorkers_)   joinWithTimeout(t, cfg_.shutdownTimeoutMs);
    for (auto& t : janitorWorkers_) joinWithTimeout(t, cfg_.shutdownTimeoutMs);

    // ── Close reactor fd ──────────────────────────────────────────────────
    if (reactorFd_ >= 0) { ::close(reactorFd_); reactorFd_ = -1; }

    // ── Drain and close all pooled fds ────────────────────────────────────
    clear();
}

// =============================================================================
// Public API — setters
// =============================================================================

void ConnectionPool::setTlsHandshake(TlsHandshakeFn fn) noexcept {
    std::lock_guard<std::mutex> lk(tlsMutex_);
    tlsFn_ = std::move(fn);
}

void ConnectionPool::setMetricsPush(MetricsPushFn fn) noexcept {
    std::lock_guard<std::mutex> lk(metricsPushMutex_);
    metricsPushFn_ = std::move(fn);
}

// =============================================================================
// Public API — acquire (synchronous)
// =============================================================================

int ConnectionPool::acquire(const std::string& host, uint16_t port) noexcept {
    const std::string key = makeKey(host, port);
    auto bucket = getBucket(key);

    // Fast path: reuse a pooled healthy slot
    {
        std::lock_guard<std::mutex> lk(bucket->mtx);
        while (!bucket->slots.empty()) {
            Slot s = bucket->slots.front();
            bucket->slots.pop();
            if (s.fd >= 0 && isAlive(s.fd)) {
                ++metAcquired_;
                return s.fd;
            }
            closeFd(s.fd);
        }
    }

    // Slow path: open a new connection on the calling thread
    if (openFds_.load() >= cfg_.globalMaxFds) {
        ++metGlobalRej_;
        ++metFailed_;
        return -1;
    }

    int fd = openBlocking(host, port);
    if (fd < 0) { ++metFailed_; return -1; }

    fd = applyTlsDirect(fd, host, port);
    if (fd < 0) { ++metFailed_; ++metTlsFail_; return -1; }

    ++metAcquired_;
    return fd;
}

// =============================================================================
// Public API — acquireAsync
// =============================================================================

void ConnectionPool::acquireAsync(const std::string& host, uint16_t port,
                                   AcquireCallback cb) noexcept {
    ++metAsyncDepth_;
    {
        std::lock_guard<std::mutex> lk(asyncMutex_);
        asyncQueue_.push({ host, port, std::move(cb) });
    }
    asyncCv_.notify_one();
}

// =============================================================================
// Public API — release
// =============================================================================

void ConnectionPool::release(const std::string& host, uint16_t port,
                              int fd, bool healthy) noexcept {
    ++metReleased_;

    if (fd < 0 || !healthy) {
        closeFd(fd);
        return;
    }

    const std::string key = makeKey(host, port);
    auto bucket = getBucket(key);

    std::lock_guard<std::mutex> lk(bucket->mtx);
    if (bucket->slots.size() >= cfg_.maxConnsPerPeer) {
        closeFd(fd);
        ++metEvicted_;
        return;
    }
    bucket->slots.push({ fd, nowSecs() });
}

// =============================================================================
// Public API — evictPeer / clear / getMetrics
// =============================================================================

void ConnectionPool::evictPeer(const std::string& host, uint16_t port) noexcept {
    const std::string key = makeKey(host, port);
    std::shared_ptr<Bucket> bucket;
    {
        std::shared_lock<std::shared_mutex> lk(mapMutex_);
        auto it = pool_.find(key);
        if (it == pool_.end()) return;
        bucket = it->second;
    }
    std::lock_guard<std::mutex> lk(bucket->mtx);
    while (!bucket->slots.empty()) {
        closeFd(bucket->slots.front().fd);
        bucket->slots.pop();
        ++metEvicted_;
    }
}

void ConnectionPool::clear() noexcept {
    std::unique_lock<std::shared_mutex> lk(mapMutex_);
    for (auto& [key, bucket] : pool_) {
        std::lock_guard<std::mutex> blk(bucket->mtx);
        while (!bucket->slots.empty()) {
            closeFd(bucket->slots.front().fd);
            bucket->slots.pop();
            ++metEvicted_;
        }
    }
    pool_.clear();
}

ConnectionPool::Metrics ConnectionPool::getMetrics() const noexcept {
    Metrics m;
    m.acquired               = metAcquired_  .load();
    m.released               = metReleased_  .load();
    m.evicted                = metEvicted_   .load();
    m.failed                 = metFailed_    .load();
    m.currentOpenFds         = openFds_      .load();
    m.globalBudgetRejections = metGlobalRej_ .load();
    m.tlsFailures            = metTlsFail_   .load();
    m.asyncQueueDepth        = metAsyncDepth_.load();
    m.reactorEvents          = metReactorEvt_.load();
    return m;
}

// =============================================================================
// Private — getBucket
// =============================================================================

std::shared_ptr<ConnectionPool::Bucket>
ConnectionPool::getBucket(const std::string& key) noexcept {
    {
        std::shared_lock<std::shared_mutex> lk(mapMutex_);
        auto it = pool_.find(key);
        if (it != pool_.end()) return it->second;
    }
    auto b = std::make_shared<Bucket>();
    std::unique_lock<std::shared_mutex> lk(mapMutex_);
    return pool_.emplace(key, std::move(b)).first->second;
}

// =============================================================================
// Private — closeFd / isAlive
// =============================================================================

void ConnectionPool::closeFd(int fd) noexcept {
    if (fd < 0) return;
    ::close(fd);
    if (openFds_.load() > 0) --openFds_;
}

bool ConnectionPool::isAlive(int fd) const noexcept {
    if (fd < 0) return false;
    char buf;
    // MSG_PEEK + MSG_DONTWAIT: returns 0 if peer closed cleanly, -1 with
    // EAGAIN/EWOULDBLOCK if alive and no data, or error if dead.
    int r = static_cast<int>(::recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT));
    if (r == 0) return false;          // EOF — peer closed
    if (r < 0 && errno == EBADF)  return false;
    if (r < 0 && errno == ENOTCONN) return false;
    return true;                       // EAGAIN or data available — alive
}

// =============================================================================
// Private — openBlocking
// =============================================================================

int ConnectionPool::openBlocking(const std::string& host, uint16_t port) noexcept {
    addrinfo* res = resolveHost(host, port);
    if (!res) return -1;

    FdGuard guard;
    uint32_t retries = 0;

    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        guard.fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (guard.fd < 0) continue;

        applySocketOptions(guard.fd);

        // Set connect timeout via SO_SNDTIMEO
        {
            struct timeval tv;
            tv.tv_sec  = cfg_.connectTimeoutMs / 1000;
            tv.tv_usec = (cfg_.connectTimeoutMs % 1000) * 1000;
            ::setsockopt(guard.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }

        retries = 0;
    retry:
        int r = ::connect(guard.fd, ai->ai_addr,
                          static_cast<socklen_t>(ai->ai_addrlen));
        if (r == 0) {
            ::freeaddrinfo(res);
            ++openFds_;
            return guard.release();
        }
        if (errno == EINTR && ++retries <= cfg_.maxEINTRRetries) goto retry;

        ::close(guard.fd);
        guard.fd = -1;
    }

    ::freeaddrinfo(res);
    return -1;
}

// =============================================================================
// Private — applyTlsDirect
// =============================================================================

int ConnectionPool::applyTlsDirect(int fd,
                                    const std::string& host,
                                    uint16_t port) noexcept {
    TlsHandshakeFn fn;
    {
        std::lock_guard<std::mutex> lk(tlsMutex_);
        fn = tlsFn_;
    }
    if (!fn) return fd;   // no TLS configured

    int tfd = fn(fd, host, port);
    if (tfd < 0) {
        ::close(fd);
        ++metTlsFail_;
        return -1;
    }
    return tfd;
}

// =============================================================================
// Reactor — init
// =============================================================================

bool ConnectionPool::initReactor() noexcept {
#if defined(POOL_USE_EPOLL)
    reactorFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    return reactorFd_ >= 0;
#elif defined(POOL_USE_KQUEUE)
    reactorFd_ = ::kqueue();
    return reactorFd_ >= 0;
#else
    reactorFd_ = 0;   // poll needs no persistent fd
    return true;
#endif
}

// =============================================================================
// Reactor — registerWrite / unregisterFd
// =============================================================================

void ConnectionPool::registerWrite(int fd, PendingConnect pc) noexcept {
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        pendingConnects_[fd] = std::move(pc);
    }

#if defined(POOL_USE_EPOLL)
    epoll_event ev{};
    ev.events   = EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLET;
    ev.data.fd  = fd;
    ::epoll_ctl(reactorFd_, EPOLL_CTL_ADD, fd, &ev);

#elif defined(POOL_USE_KQUEUE)
    struct kevent ev{};
    EV_SET(&ev, static_cast<uintptr_t>(fd),
           EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, nullptr);
    ::kevent(reactorFd_, &ev, 1, nullptr, 0, nullptr);
#endif
    // poll: handled in runReactor by iterating pendingConnects_
}

void ConnectionPool::unregisterFd(int fd) noexcept {
#if defined(POOL_USE_EPOLL)
    ::epoll_ctl(reactorFd_, EPOLL_CTL_DEL, fd, nullptr);
#elif defined(POOL_USE_KQUEUE)
    struct kevent ev{};
    EV_SET(&ev, static_cast<uintptr_t>(fd),
           EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(reactorFd_, &ev, 1, nullptr, 0, nullptr);
#endif
    std::lock_guard<std::mutex> lk(pendingMutex_);
    pendingConnects_.erase(fd);
}

// =============================================================================
// Reactor — openAsync
// =============================================================================

void ConnectionPool::openAsync(const std::string& host, uint16_t port,
                                AcquireCallback cb) noexcept {
    // DNS resolution is pushed to a DNS worker; after resolution the
    // non-blocking connect fd is registered with the reactor.
    {
        std::lock_guard<std::mutex> lk(dnsMutex_);
        dnsQueue_.push({ host, port, std::move(cb) });
    }
    dnsCv_.notify_one();
}

// =============================================================================
// Reactor — runReactor
// =============================================================================

void ConnectionPool::runReactor() noexcept {
    constexpr int kTimeoutMs = 200;

#if defined(POOL_USE_EPOLL)
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    while (reactorRunning_.load()) {
        int n = ::epoll_wait(reactorFd_, events, kMaxEvents, kTimeoutMs);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            ++metReactorEvt_;
            bool err = (events[i].events & (EPOLLERR | EPOLLHUP)) != 0;
            handleReactorEvent(events[i].data.fd, err);
        }
        // Timeout-expired pending connects
        {
            std::lock_guard<std::mutex> lk(pendingMutex_);
            uint64_t now = nowMs();
            std::vector<int> expired;
            for (auto& [fd, pc] : pendingConnects_)
                if (now >= pc.deadlineMs) expired.push_back(fd);
            for (int fd : expired) {
                auto it = pendingConnects_.find(fd);
                if (it == pendingConnects_.end()) continue;
                AcquireCallback cb = std::move(it->second.cb);
                pendingConnects_.erase(it);
                ::epoll_ctl(reactorFd_, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                if (openFds_.load() > 0) --openFds_;
                cb(-1);
            }
        }
    }

#elif defined(POOL_USE_KQUEUE)
    constexpr int kMaxEvents = 64;
    struct kevent events[kMaxEvents];
    struct timespec ts{ 0, kTimeoutMs * 1'000'000L };

    while (reactorRunning_.load()) {
        int n = ::kevent(reactorFd_, nullptr, 0, events, kMaxEvents, &ts);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            ++metReactorEvt_;
            bool err = (events[i].flags & EV_ERROR) != 0;
            handleReactorEvent(static_cast<int>(events[i].ident), err);
        }
        // Timeout check
        {
            std::lock_guard<std::mutex> lk(pendingMutex_);
            uint64_t now = nowMs();
            std::vector<int> expired;
            for (auto& [fd, pc] : pendingConnects_)
                if (now >= pc.deadlineMs) expired.push_back(fd);
            for (int fd : expired) {
                auto it = pendingConnects_.find(fd);
                if (it == pendingConnects_.end()) continue;
                AcquireCallback cb = std::move(it->second.cb);
                pendingConnects_.erase(it);
                ::close(fd);
                if (openFds_.load() > 0) --openFds_;
                cb(-1);
            }
        }
    }

#else   // POLL fallback
    while (reactorRunning_.load()) {
        std::vector<pollfd>       pfds;
        std::vector<int>          fds;
        {
            std::lock_guard<std::mutex> lk(pendingMutex_);
            pfds.reserve(pendingConnects_.size());
            fds .reserve(pendingConnects_.size());
            for (auto& [fd, _] : pendingConnects_) {
                pfds.push_back({ fd, POLLOUT, 0 });
                fds.push_back(fd);
            }
        }
        if (pfds.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kTimeoutMs));
            continue;
        }
        int n = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), kTimeoutMs);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (size_t i = 0; i < pfds.size(); ++i) {
            if (pfds[i].revents == 0) continue;
            ++metReactorEvt_;
            bool err = (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            handleReactorEvent(fds[i], err);
        }
        // Timeout check
        {
            std::lock_guard<std::mutex> lk(pendingMutex_);
            uint64_t now = nowMs();
            std::vector<int> expired;
            for (auto& [fd, pc] : pendingConnects_)
                if (now >= pc.deadlineMs) expired.push_back(fd);
            for (int fd : expired) {
                auto it = pendingConnects_.find(fd);
                if (it == pendingConnects_.end()) continue;
                AcquireCallback cb = std::move(it->second.cb);
                pendingConnects_.erase(it);
                ::close(fd);
                if (openFds_.load() > 0) --openFds_;
                cb(-1);
            }
        }
    }
#endif
}

// =============================================================================
// Reactor — handleReactorEvent
// =============================================================================

void ConnectionPool::handleReactorEvent(int fd, bool error) noexcept {
    PendingConnect pc;
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        auto it = pendingConnects_.find(fd);
        if (it == pendingConnects_.end()) return;
        pc = std::move(it->second);
        pendingConnects_.erase(it);
    }

#if defined(POOL_USE_EPOLL)
    ::epoll_ctl(reactorFd_, EPOLL_CTL_DEL, fd, nullptr);
#endif

    if (error) {
        ::close(fd);
        if (openFds_.load() > 0) --openFds_;
        ++metFailed_;
        pc.cb(-1);
        return;
    }

    // Verify connect actually succeeded via getsockopt
    int       err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        ::close(fd);
        if (openFds_.load() > 0) --openFds_;
        ++metFailed_;
        pc.cb(-1);
        return;
    }

    // Back to blocking before handing to TLS worker
    setBlocking(fd);

    // Push to TLS worker (or complete directly if no TLS)
    {
        std::lock_guard<std::mutex> lk(tlsMutex_);
        if (!tlsFn_) {
            ++metAcquired_;
            pc.cb(fd);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lk(tlsQueueMutex_);
        tlsQueue_.push({ fd, pc.host, pc.port, std::move(pc.cb) });
    }
    tlsCv_.notify_one();
}

// =============================================================================
// Worker loops
// =============================================================================

void ConnectionPool::runDnsWorker() noexcept {
    while (dnsRunning_.load()) {
        DnsRequest req;
        {
            std::unique_lock<std::mutex> lk(dnsMutex_);
            dnsCv_.wait(lk, [this] {
                return !dnsQueue_.empty() || !dnsRunning_.load();
            });
            if (!dnsRunning_.load() && dnsQueue_.empty()) return;
            req = std::move(dnsQueue_.front());
            dnsQueue_.pop();
        }

        // Global budget check
        if (openFds_.load() >= cfg_.globalMaxFds) {
            ++metGlobalRej_;
            ++metFailed_;
            req.cb(-1);
            continue;
        }

        // DNS resolution (blocking — on this worker thread, not the caller)
        addrinfo* res = resolveHost(req.host, req.port);
        if (!res) {
            ++metFailed_;
            req.cb(-1);
            continue;
        }

        // Create non-blocking socket and initiate connect
        int fd = -1;
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;

            applySocketOptions(fd);
            setNonBlocking(fd);

            int r = ::connect(fd, ai->ai_addr,
                              static_cast<socklen_t>(ai->ai_addrlen));
            if (r == 0 || errno == EINPROGRESS) {
                // Register with reactor
                ++openFds_;
                PendingConnect pc;
                pc.fd         = fd;
                pc.host       = req.host;
                pc.port       = req.port;
                pc.deadlineMs = nowMs() + cfg_.connectTimeoutMs;
                pc.cb         = std::move(req.cb);
                registerWrite(fd, std::move(pc));
                fd = -1;
                break;
            }
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(res);

        if (fd >= 0) {
            // All addresses failed immediately
            ::close(fd);
            ++metFailed_;
            req.cb(-1);
        }
    }
}

void ConnectionPool::runTlsWorker() noexcept {
    while (tlsRunning_.load()) {
        TlsRequest req;
        {
            std::unique_lock<std::mutex> lk(tlsQueueMutex_);
            tlsCv_.wait(lk, [this] {
                return !tlsQueue_.empty() || !tlsRunning_.load();
            });
            if (!tlsRunning_.load() && tlsQueue_.empty()) return;
            req = std::move(tlsQueue_.front());
            tlsQueue_.pop();
        }

        TlsHandshakeFn fn;
        {
            std::lock_guard<std::mutex> lk(tlsMutex_);
            fn = tlsFn_;
        }

        int finalFd = req.fd;
        if (fn) {
            finalFd = fn(req.fd, req.host, req.port);
            if (finalFd < 0) {
                ::close(req.fd);
                ++metTlsFail_;
                ++metFailed_;
                req.cb(-1);
                continue;
            }
        }

        ++metAcquired_;
        req.cb(finalFd);
    }
}

void ConnectionPool::runAsyncWorker() noexcept {
    while (asyncRunning_.load()) {
        AsyncRequest req;
        {
            std::unique_lock<std::mutex> lk(asyncMutex_);
            asyncCv_.wait(lk, [this] {
                return !asyncQueue_.empty() || !asyncRunning_.load();
            });
            if (!asyncRunning_.load() && asyncQueue_.empty()) return;
            req = std::move(asyncQueue_.front());
            asyncQueue_.pop();
        }
        if (metAsyncDepth_.load() > 0) --metAsyncDepth_;

        // Fast path: reuse pooled connection
        const std::string key = makeKey(req.host, req.port);
        auto bucket = getBucket(key);
        {
            std::lock_guard<std::mutex> lk(bucket->mtx);
            while (!bucket->slots.empty()) {
                Slot s = bucket->slots.front();
                bucket->slots.pop();
                if (s.fd >= 0 && isAlive(s.fd)) {
                    ++metAcquired_;
                    req.cb(s.fd);
                    goto next;
                }
                closeFd(s.fd);
            }
        }

        // Slow path: open asynchronously via DNS worker → reactor → TLS worker
        openAsync(req.host, req.port, std::move(req.cb));
    next:;
    }
}

void ConnectionPool::runJanitorWorker(size_t index) noexcept {
    while (janitorRunning_.load()) {
        {
            std::unique_lock<std::mutex> lk(janitorMutex_);
            janitorCv_.wait_for(lk,
                std::chrono::seconds(cfg_.janitorIntervalSecs),
                [this] { return !janitorRunning_.load(); });
        }
        if (!janitorRunning_.load()) return;

        // Each janitor worker processes a shard of the map
        std::vector<std::shared_ptr<Bucket>> buckets;
        {
            std::shared_lock<std::shared_mutex> lk(mapMutex_);
            buckets.reserve(pool_.size());
            size_t idx = 0;
            for (auto& [_, b] : pool_) {
                if (idx++ % cfg_.janitorWorkerCount == index)
                    buckets.push_back(b);
            }
        }

        const uint64_t now = nowSecs();
        for (auto& bucket : buckets) {
            std::lock_guard<std::mutex> blk(bucket->mtx);
            std::queue<Slot> kept;
            while (!bucket->slots.empty()) {
                Slot s = bucket->slots.front();
                bucket->slots.pop();
                if (s.fd >= 0
                    && (now - s.lastUsed) < cfg_.idleTimeoutSecs
                    && isAlive(s.fd))
                {
                    kept.push(s);
                } else {
                    closeFd(s.fd);
                    ++metEvicted_;
                }
            }
            bucket->slots = std::move(kept);
        }
    }
}

void ConnectionPool::runMetricsReporter() noexcept {
    while (metricsRunning_.load()) {
        {
            std::unique_lock<std::mutex> lk(metricsMutex_);
            metricsCv_.wait_for(lk,
                std::chrono::milliseconds(cfg_.metricsIntervalMs),
                [this] { return !metricsRunning_.load(); });
        }
        if (!metricsRunning_.load()) return;

        MetricsPushFn fn;
        {
            std::lock_guard<std::mutex> lk(metricsPushMutex_);
            fn = metricsPushFn_;
        }
        if (fn) fn(getMetrics());
    }
}
