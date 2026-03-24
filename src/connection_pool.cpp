#include "connection_pool.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// ── Platform I/O multiplexer selection ────────────────────────────────────────
#if defined(__linux__)
#  include <sys/epoll.h>
#  define MEDOR_EPOLL  1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  include <sys/event.h>
#  define MEDOR_KQUEUE 1
#else
#  include <poll.h>
#  define MEDOR_POLL   1
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

std::string ConnectionPool::makeKey(const std::string &h, uint16_t p) noexcept
{
    return std::to_string(h.size()) + ":" + h + ":" + std::to_string(p);
}

uint64_t ConnectionPool::nowSecs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t ConnectionPool::nowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void ConnectionPool::closeFd(int fd) noexcept
{
    if (fd < 0) return;
    ::close(fd);
    if (openFds_.load(std::memory_order_relaxed) > 0)
        openFds_.fetch_sub(1, std::memory_order_relaxed);
}

void ConnectionPool::joinWithTimeout(std::thread &t, uint32_t ms) noexcept
{
    if (!t.joinable()) return;
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future();
    std::thread helper([th = std::move(t), pp = p]() mutable {
        th.join(); pp->set_value();
    });
    helper.detach();
    f.wait_for(std::chrono::milliseconds(ms));
}

// ─────────────────────────────────────────────────────────────────────────────
// applySocketOptions
//
// Platform policy applied uniformly:
//   - TCP_NODELAY:   always — eliminates Nagle delay for blockchain messages.
//   - SO_KEEPALIVE:  always — enables OS-level dead-connection detection.
//   - Idle timeout:  60 s  — use TCP_KEEPIDLE (Linux/modern macOS),
//                            TCP_KEEPALIVE (older macOS), or omit if absent.
//   - Probe interval: 10 s — TCP_KEEPINTVL where available.
//   - Probe count:    3    — TCP_KEEPCNT where available.
//   - SO_NOSIGPIPE:  macOS only — suppresses SIGPIPE at the socket level;
//                    on Linux MSG_NOSIGNAL is used per-send instead.
//   - TCP_USER_TIMEOUT: Linux only — 30 s unacknowledged data timeout;
//                       tighter than keepalive for already-established
//                       connections under data transfer.
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::applySocketOptions(int fd) noexcept
{
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,  &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &one, sizeof(one));

#if defined(TCP_KEEPIDLE)
    int keepIdle = 60;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(keepIdle));
#elif defined(TCP_KEEPALIVE)
    int keepAlive = 60;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &keepAlive, sizeof(keepAlive));
#endif

#if defined(TCP_KEEPINTVL)
    int keepIntvl = 10;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepIntvl, sizeof(keepIntvl));
#endif

#if defined(TCP_KEEPCNT)
    int keepCnt = 3;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepCnt, sizeof(keepCnt));
#endif

#if defined(SO_NOSIGPIPE)
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

#if defined(TCP_USER_TIMEOUT) && defined(__linux__)
    unsigned int userTimeout = 30000;
    ::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
                 &userTimeout, sizeof(userTimeout));
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Reactor initialisation
// ─────────────────────────────────────────────────────────────────────────────

bool ConnectionPool::initReactor() noexcept
{
#if defined(MEDOR_EPOLL)
    reactorFd_ = ::epoll_create1(EPOLL_CLOEXEC);
#elif defined(MEDOR_KQUEUE)
    reactorFd_ = ::kqueue();
#else
    reactorFd_ = 0;
#endif
    if (reactorFd_ < 0) {
        std::cerr << "[ConnectionPool] initReactor: failed — "
                  << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

void ConnectionPool::registerWrite(int fd, PendingConnect pc) noexcept
{
    {
        std::unique_lock<std::mutex> lock(pendingMutex_);
        pendingConnects_.emplace(fd, std::move(pc));
    }

#if defined(MEDOR_EPOLL)
    struct epoll_event ev{};
    ev.events  = EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLET;
    ev.data.fd = fd;
    ::epoll_ctl(reactorFd_, EPOLL_CTL_ADD, fd, &ev);

#elif defined(MEDOR_KQUEUE)
    struct kevent change{};
    EV_SET(&change, fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, nullptr);
    struct timespec zero{ 0, 0 };
    ::kevent(reactorFd_, &change, 1, nullptr, 0, &zero);
#endif
}

void ConnectionPool::unregisterFd(int fd) noexcept
{
#if defined(MEDOR_EPOLL)
    ::epoll_ctl(reactorFd_, EPOLL_CTL_DEL, fd, nullptr);
#elif defined(MEDOR_KQUEUE)
    struct kevent change{};
    EV_SET(&change, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    struct timespec zero{ 0, 0 };
    ::kevent(reactorFd_, &change, 1, nullptr, 0, &zero);
#endif
    std::unique_lock<std::mutex> lock(pendingMutex_);
    pendingConnects_.erase(fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// handleReactorEvent
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::handleReactorEvent(int fd, bool error) noexcept
{
    PendingConnect pc;
    {
        std::unique_lock<std::mutex> lock(pendingMutex_);
        auto it = pendingConnects_.find(fd);
        if (it == pendingConnects_.end()) return;
        pc = std::move(it->second);
        pendingConnects_.erase(it);
    }

    unregisterFd(fd);

    if (error) {
        closeFd(fd);
        metFailed_.fetch_add(1, std::memory_order_relaxed);
        if (pc.cb) try { pc.cb(-1); } catch (...) {}
        return;
    }

    int err = 0; socklen_t el = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) {
        closeFd(fd);
        metFailed_.fetch_add(1, std::memory_order_relaxed);
        if (pc.cb) try { pc.cb(-1); } catch (...) {}
        return;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    applySocketOptions(fd);

    metReactorEvt_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(tlsMutex_);
        if (tlsFn_) {
            {
                std::unique_lock<std::mutex> qlock(tlsQueueMutex_);
                tlsQueue_.push({ fd, pc.host, pc.port, std::move(pc.cb) });
            }
            tlsCv_.notify_one();
            return;
        }
    }

    openFds_.fetch_add(1, std::memory_order_relaxed);
    metAcquired_.fetch_add(1, std::memory_order_relaxed);
    if (pc.cb) try { pc.cb(fd); } catch (...) { closeFd(fd); }
}

// ─────────────────────────────────────────────────────────────────────────────
// runReactor
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::runReactor() noexcept
{
    while (reactorRunning_.load()) {

#if defined(MEDOR_EPOLL)
        constexpr int MAX_EVENTS = 64;
        struct epoll_event events[MAX_EVENTS];
        int n = ::epoll_wait(reactorFd_, events, MAX_EVENTS, 50);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; ++i) {
            const bool err = (events[i].events & (EPOLLERR | EPOLLHUP)) != 0;
            handleReactorEvent(events[i].data.fd, err);
        }

#elif defined(MEDOR_KQUEUE)
        constexpr int MAX_EVENTS = 64;
        struct kevent events[MAX_EVENTS];
        struct timespec ts{ 0, 50'000'000L };
        int n = ::kevent(reactorFd_, nullptr, 0, events, MAX_EVENTS, &ts);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; ++i) {
            const bool err = (events[i].flags & EV_ERROR) != 0;
            handleReactorEvent(static_cast<int>(events[i].ident), err);
        }

#else
        std::vector<pollfd> pfds;
        std::vector<int>    fdList;
        {
            std::unique_lock<std::mutex> lock(pendingMutex_);
            pfds.reserve(pendingConnects_.size());
            fdList.reserve(pendingConnects_.size());
            for (const auto &[fd, _] : pendingConnects_) {
                pfds.push_back({ fd, POLLOUT, 0 });
                fdList.push_back(fd);
            }
        }
        if (pfds.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        int n = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 50);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (size_t i = 0; i < pfds.size(); ++i) {
            if (pfds[i].revents == 0) continue;
            const bool err = (pfds[i].revents & (POLLERR | POLLHUP)) != 0;
            handleReactorEvent(fdList[i], err);
        }
#endif

        const uint64_t now = nowMs();
        std::vector<int> timedOut;
        {
            std::unique_lock<std::mutex> lock(pendingMutex_);
            for (const auto &[fd, pc] : pendingConnects_)
                if (pc.deadlineMs > 0 && now > pc.deadlineMs)
                    timedOut.push_back(fd);
        }
        for (int fd : timedOut) {
            std::cerr << "[ConnectionPool] reactor: connect timeout fd="
                      << fd << "\n";
            handleReactorEvent(fd, true);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// openAsync
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::openAsync(const std::string &host,
                                uint16_t           port,
                                AcquireCallback    cb) noexcept
{
    if (openFds_.load(std::memory_order_relaxed) >= cfg_.globalMaxFds) {
        metGlobalRej_.fetch_add(1, std::memory_order_relaxed);
        if (cb) try { cb(-1); } catch (...) {}
        return;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICSERV | AI_ADDRCONFIG;

    const std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        metFailed_.fetch_add(1, std::memory_order_relaxed);
        if (cb) try { cb(-1); } catch (...) {}
        return;
    }

    FdGuard guard;
    bool registered = false;

    for (struct addrinfo *ai = res; ai && !registered; ai = ai->ai_next) {
        guard = FdGuard(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (guard.fd < 0) continue;

        int flags = ::fcntl(guard.fd, F_GETFL, 0);
        if (flags < 0) continue;
        ::fcntl(guard.fd, F_SETFL, flags | O_NONBLOCK);

        int rc = ::connect(guard.fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) continue;

        PendingConnect pc;
        pc.fd         = guard.fd;
        pc.host       = host;
        pc.port       = port;
        pc.deadlineMs = nowMs() + cfg_.connectTimeoutMs;
        pc.cb         = std::move(cb);

        registerWrite(guard.release(), std::move(pc));
        registered = true;
    }
    ::freeaddrinfo(res);

    if (!registered) {
        metFailed_.fetch_add(1, std::memory_order_relaxed);
        if (cb) try { cb(-1); } catch (...) {}
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// openBlocking
// ─────────────────────────────────────────────────────────────────────────────

int ConnectionPool::openBlocking(const std::string &host,
                                  uint16_t           port) noexcept
{
    if (openFds_.load(std::memory_order_relaxed) >= cfg_.globalMaxFds) {
        metGlobalRej_.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICSERV | AI_ADDRCONFIG;

    const std::string portStr = std::to_string(port);
    uint32_t delayMs = cfg_.retryBaseDelayMs;

    for (uint32_t attempt = 0; attempt <= cfg_.maxConnectRetries; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            delayMs = std::min(delayMs * 2u, uint32_t{ 8000 });
        }

        res = nullptr;
        if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
            continue;

        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            FdGuard guard(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
            if (guard.fd < 0) continue;

            int flags = ::fcntl(guard.fd, F_GETFL, 0);
            if (flags < 0) continue;
            ::fcntl(guard.fd, F_SETFL, flags | O_NONBLOCK);

            int rc = ::connect(guard.fd, ai->ai_addr, ai->ai_addrlen);
            if (rc != 0 && errno != EINPROGRESS) continue;

            uint32_t waited = 0;
            bool writable   = false;
            while (waited < cfg_.connectTimeoutMs) {
                uint32_t slice = std::min(50u, cfg_.connectTimeoutMs - waited);

#if defined(MEDOR_EPOLL)
                fd_set wfds; FD_ZERO(&wfds); FD_SET(guard.fd, &wfds);
                struct timeval tv;
                tv.tv_sec  = slice / 1000;
                tv.tv_usec = (slice % 1000) * 1000;
                int sel = ::select(guard.fd + 1, nullptr, &wfds, nullptr, &tv);
                if (sel > 0) { writable = true; break; }
                if (sel < 0 && errno != EINTR) break;
#elif defined(MEDOR_KQUEUE)
                struct pollfd pfd{ guard.fd, POLLOUT, 0 };
                int pr = ::poll(&pfd, 1, static_cast<int>(slice));
                if (pr > 0) { writable = true; break; }
                if (pr < 0 && errno != EINTR) break;
#else
                struct pollfd pfd{ guard.fd, POLLOUT, 0 };
                int pr = ::poll(&pfd, 1, static_cast<int>(slice));
                if (pr > 0) { writable = true; break; }
                if (pr < 0 && errno != EINTR) break;
#endif
                waited += slice;
            }
            if (!writable) continue;

            int err = 0; socklen_t el = sizeof(err);
            if (::getsockopt(guard.fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0)
                continue;

            ::fcntl(guard.fd, F_SETFL, flags & ~O_NONBLOCK);
            applySocketOptions(guard.fd);

            int finalFd = applyTlsDirect(guard.fd, host, port);
            if (finalFd < 0) continue;
            if (finalFd != guard.fd) guard.release();
            else                     guard.release();

            ::freeaddrinfo(res);
            openFds_.fetch_add(1, std::memory_order_relaxed);
            return finalFd;
        }
        ::freeaddrinfo(res);
    }

    metFailed_.fetch_add(1, std::memory_order_relaxed);
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// applyTlsDirect
// ─────────────────────────────────────────────────────────────────────────────

int ConnectionPool::applyTlsDirect(int fd,
                                    const std::string &host,
                                    uint16_t           port) noexcept
{
    std::lock_guard<std::mutex> lock(tlsMutex_);
    if (!tlsFn_) return fd;
    int result = -1;
    try {
        result = tlsFn_(fd, host, port);
    } catch (const std::exception &e) {
        std::cerr << "[ConnectionPool] TLS threw: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[ConnectionPool] TLS threw unknown\n";
    }
    if (result < 0) metTlsFail_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// isAlive
// ─────────────────────────────────────────────────────────────────────────────

bool ConnectionPool::isAlive(int fd) const noexcept
{
    if (fd < 0) return false;
    int err = 0; socklen_t el = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0)
        return false;
    for (uint32_t r = 0; r <= cfg_.maxEINTRRetries; ++r) {
        ssize_t rc = ::send(fd, "", 0, MSG_NOSIGNAL);
        if (rc == 0)        return true;
        if (errno == EINTR) continue;
        return false;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// getBucket
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<ConnectionPool::Bucket>
ConnectionPool::getBucket(const std::string &key) noexcept
{
    {
        std::shared_lock<std::shared_mutex> rlock(mapMutex_);
        auto it = pool_.find(key);
        if (it != pool_.end()) return it->second;
    }
    std::unique_lock<std::shared_mutex> wlock(mapMutex_);
    auto &slot = pool_[key];
    if (!slot) slot = std::make_shared<Bucket>();
    return slot;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

ConnectionPool::ConnectionPool(Config cfg)
    : cfg_(std::move(cfg))
{
    if (!initReactor()) {
        std::cerr << "[ConnectionPool] FATAL: reactor init failed\n";
        return;
    }

    reactorRunning_.store(true);
    reactorThread_ = std::thread([this]() { runReactor(); });

    dnsRunning_.store(true);
    dnsWorkers_.reserve(cfg_.dnsWorkerCount);
    for (size_t i = 0; i < cfg_.dnsWorkerCount; ++i)
        dnsWorkers_.emplace_back([this]() { runDnsWorker(); });

    tlsRunning_.store(true);
    tlsWorkers_.reserve(cfg_.tlsWorkerCount);
    for (size_t i = 0; i < cfg_.tlsWorkerCount; ++i)
        tlsWorkers_.emplace_back([this]() { runTlsWorker(); });

    asyncRunning_.store(true);
    asyncWorkers_.reserve(cfg_.asyncWorkerCount);
    for (size_t i = 0; i < cfg_.asyncWorkerCount; ++i)
        asyncWorkers_.emplace_back([this]() { runAsyncWorker(); });

    janitorRunning_.store(true);
    janitorWorkers_.reserve(cfg_.janitorWorkerCount);
    for (size_t i = 0; i < cfg_.janitorWorkerCount; ++i)
        janitorWorkers_.emplace_back([this, i]() { runJanitorWorker(i); });

    if (cfg_.metricsIntervalMs > 0) {
        metricsRunning_.store(true);
        metricsThread_ = std::thread([this]() { runMetricsReporter(); });
    }
}

ConnectionPool::~ConnectionPool()
{
    reactorRunning_.store(false);
    if (reactorFd_ >= 0) {
#if defined(MEDOR_EPOLL) || defined(MEDOR_KQUEUE)
        ::close(reactorFd_);
        reactorFd_ = -1;
#endif
    }
    joinWithTimeout(reactorThread_, cfg_.shutdownTimeoutMs);

    dnsRunning_.store(false);
    dnsCv_.notify_all();
    for (auto &t : dnsWorkers_) joinWithTimeout(t, cfg_.shutdownTimeoutMs);

    tlsRunning_.store(false);
    tlsCv_.notify_all();
    for (auto &t : tlsWorkers_) joinWithTimeout(t, cfg_.shutdownTimeoutMs);

    asyncRunning_.store(false);
    asyncCv_.notify_all();
    for (auto &t : asyncWorkers_) joinWithTimeout(t, cfg_.shutdownTimeoutMs);

    janitorRunning_.store(false);
    janitorCv_.notify_all();
    for (auto &t : janitorWorkers_) joinWithTimeout(t, cfg_.shutdownTimeoutMs);

    metricsRunning_.store(false);
    metricsCv_.notify_all();
    joinWithTimeout(metricsThread_, cfg_.shutdownTimeoutMs);

    clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Callback setters
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::setTlsHandshake(TlsHandshakeFn fn) noexcept
{
    std::lock_guard<std::mutex> lock(tlsMutex_);
    tlsFn_ = std::move(fn);
}

void ConnectionPool::setMetricsPush(MetricsPushFn fn) noexcept
{
    std::lock_guard<std::mutex> lock(metricsPushMutex_);
    metricsPushFn_ = std::move(fn);
}

// ─────────────────────────────────────────────────────────────────────────────
// acquire
// ─────────────────────────────────────────────────────────────────────────────

int ConnectionPool::acquire(const std::string &host, uint16_t port) noexcept
{
    const std::string k      = makeKey(host, port);
    const uint64_t    cutoff = nowSecs() - cfg_.idleTimeoutSecs;
    auto bucket              = getBucket(k);

    {
        std::unique_lock<std::mutex> lock(bucket->mtx);
        while (!bucket->slots.empty()) {
            Slot slot = bucket->slots.front();
            bucket->slots.pop();
            if (slot.lastUsed < cutoff || !isAlive(slot.fd)) {
                closeFd(slot.fd);
                metEvicted_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            metAcquired_.fetch_add(1, std::memory_order_relaxed);
            return slot.fd;
        }
    }

    int fd = openBlocking(host, port);
    if (fd >= 0) metAcquired_.fetch_add(1, std::memory_order_relaxed);
    return fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// acquireAsync
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::acquireAsync(const std::string &host,
                                   uint16_t           port,
                                   AcquireCallback    cb) noexcept
{
    if (!cb) return;

    AcquireCallback cbCopy = cb;

    const std::string k      = makeKey(host, port);
    const uint64_t    cutoff = nowSecs() - cfg_.idleTimeoutSecs;
    auto bucket              = getBucket(k);

    {
        std::unique_lock<std::mutex> lock(bucket->mtx);
        while (!bucket->slots.empty()) {
            Slot slot = bucket->slots.front();
            bucket->slots.pop();
            if (slot.lastUsed < cutoff || !isAlive(slot.fd)) {
                closeFd(slot.fd);
                metEvicted_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            metAcquired_.fetch_add(1, std::memory_order_relaxed);
            const int pooledFd = slot.fd;
            {
                std::unique_lock<std::mutex> qlock(dnsMutex_);
                DnsRequest req2;
                req2.host = std::string{};
                req2.port = 0;
                req2.cb   = [pooledFd, cbCopy](){ cbCopy(pooledFd); };
                dnsQueue_.push(std::move(req2));
            }
            dnsCv_.notify_one();
            return;
        }
    }

    {
        std::unique_lock<std::mutex> lock(dnsMutex_);
        dnsQueue_.push({ host, port, std::move(cb) });
        metAsyncDepth_.fetch_add(1, std::memory_order_relaxed);
    }
    dnsCv_.notify_one();
}

// ─────────────────────────────────────────────────────────────────────────────
// release
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::release(const std::string &host, uint16_t port,
                               int fd, bool healthy) noexcept
{
    if (fd < 0) return;
    if (!healthy) { closeFd(fd); return; }

    auto bucket = getBucket(makeKey(host, port));
    {
        std::unique_lock<std::mutex> lock(bucket->mtx);
        if (bucket->slots.size() >= cfg_.maxConnsPerPeer) {
            lock.unlock();
            closeFd(fd);
            return;
        }
        bucket->slots.push({ fd, nowSecs() });
    }
    metReleased_.fetch_add(1, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// evictPeer / clear
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::evictPeer(const std::string &host, uint16_t port) noexcept
{
    const std::string k = makeKey(host, port);
    std::shared_ptr<Bucket> bucket;
    {
        std::shared_lock<std::shared_mutex> rlock(mapMutex_);
        auto it = pool_.find(k);
        if (it == pool_.end()) return;
        bucket = it->second;
    }
    {
        std::unique_lock<std::mutex> lock(bucket->mtx);
        while (!bucket->slots.empty()) {
            closeFd(bucket->slots.front().fd);
            bucket->slots.pop();
            metEvicted_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::unique_lock<std::shared_mutex> wlock(mapMutex_);
    pool_.erase(k);
}

void ConnectionPool::clear() noexcept
{
    std::unordered_map<std::string, std::shared_ptr<Bucket>> snapshot;
    {
        std::unique_lock<std::shared_mutex> wlock(mapMutex_);
        snapshot = std::move(pool_);
        pool_.clear();
    }
    for (auto &[k, bucket] : snapshot) {
        std::unique_lock<std::mutex> lock(bucket->mtx);
        while (!bucket->slots.empty()) {
            closeFd(bucket->slots.front().fd);
            bucket->slots.pop();
            metEvicted_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// getMetrics
// ─────────────────────────────────────────────────────────────────────────────

ConnectionPool::Metrics ConnectionPool::getMetrics() const noexcept
{
    Metrics m;
    m.acquired               = metAcquired_.load(std::memory_order_relaxed);
    m.released               = metReleased_.load(std::memory_order_relaxed);
    m.evicted                = metEvicted_.load(std::memory_order_relaxed);
    m.failed                 = metFailed_.load(std::memory_order_relaxed);
    m.currentOpenFds         = openFds_.load(std::memory_order_relaxed);
    m.globalBudgetRejections = metGlobalRej_.load(std::memory_order_relaxed);
    m.tlsFailures            = metTlsFail_.load(std::memory_order_relaxed);
    m.asyncQueueDepth        = metAsyncDepth_.load(std::memory_order_relaxed);
    m.reactorEvents          = metReactorEvt_.load(std::memory_order_relaxed);
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// runDnsWorker
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::runDnsWorker() noexcept
{
    while (dnsRunning_.load()) {
        DnsRequest req;
        {
            std::unique_lock<std::mutex> lock(dnsMutex_);
            dnsCv_.wait(lock, [this]() {
                return !dnsRunning_.load() || !dnsQueue_.empty();
            });
            if (!dnsRunning_.load() && dnsQueue_.empty()) return;
            req = std::move(dnsQueue_.front());
            dnsQueue_.pop();
        }

        if (req.host.empty()) {
            if (req.cb) try { req.cb(-1); } catch (...) {}
            continue;
        }

        if (metAsyncDepth_.load(std::memory_order_relaxed) > 0)
            metAsyncDepth_.fetch_sub(1, std::memory_order_relaxed);

        openAsync(req.host, req.port, std::move(req.cb));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// runTlsWorker
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::runTlsWorker() noexcept
{
    while (tlsRunning_.load()) {
        TlsRequest req;
        {
            std::unique_lock<std::mutex> lock(tlsQueueMutex_);
            tlsCv_.wait(lock, [this]() {
                return !tlsRunning_.load() || !tlsQueue_.empty();
            });
            if (!tlsRunning_.load() && tlsQueue_.empty()) return;
            req = std::move(tlsQueue_.front());
            tlsQueue_.pop();
        }

        int finalFd = applyTlsDirect(req.fd, req.host, req.port);
        if (finalFd < 0) {
            closeFd(req.fd);
            if (req.cb) try { req.cb(-1); } catch (...) {}
            continue;
        }

        openFds_.fetch_add(1, std::memory_order_relaxed);
        metAcquired_.fetch_add(1, std::memory_order_relaxed);
        if (req.cb) try { req.cb(finalFd); } catch (...) { closeFd(finalFd); }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// runAsyncWorker
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::runAsyncWorker() noexcept
{
    while (asyncRunning_.load()) {
        AsyncRequest req;
        {
            std::unique_lock<std::mutex> lock(asyncMutex_);
            asyncCv_.wait(lock, [this]() {
                return !asyncRunning_.load() || !asyncQueue_.empty();
            });
            if (!asyncRunning_.load() && asyncQueue_.empty()) return;
            req = std::move(asyncQueue_.front());
            asyncQueue_.pop();
        }
        int fd = acquire(req.host, req.port);
        try { req.cb(fd); }
        catch (const std::exception &e) {
            std::cerr << "[ConnectionPool] async cb threw: " << e.what() << "\n";
            if (fd >= 0) release(req.host, req.port, fd, false);
        } catch (...) {
            std::cerr << "[ConnectionPool] async cb threw unknown\n";
            if (fd >= 0) release(req.host, req.port, fd, false);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// runJanitorWorker
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::runJanitorWorker(size_t /*idx*/) noexcept
{
    while (janitorRunning_.load()) {
        {
            std::unique_lock<std::mutex> lock(janitorMutex_);
            janitorCv_.wait_for(lock,
                std::chrono::seconds(cfg_.janitorIntervalSecs),
                [this]() { return !janitorRunning_.load(); });
        }
        if (!janitorRunning_.load()) return;

        const uint64_t cutoff = nowSecs() - cfg_.idleTimeoutSecs;
        std::vector<std::string> keys;
        {
            std::shared_lock<std::shared_mutex> rlock(mapMutex_);
            keys.reserve(pool_.size());
            for (const auto &[k, _] : pool_) keys.push_back(k);
        }
        for (const auto &k : keys) {
            std::shared_ptr<Bucket> bucket;
            {
                std::shared_lock<std::shared_mutex> rlock(mapMutex_);
                auto it = pool_.find(k);
                if (it == pool_.end()) continue;
                bucket = it->second;
            }
            std::unique_lock<std::mutex> lock(bucket->mtx, std::try_to_lock);
            if (!lock.owns_lock()) continue;
            std::queue<Slot> survivors;
            while (!bucket->slots.empty()) {
                Slot slot = bucket->slots.front(); bucket->slots.pop();
                if (slot.lastUsed < cutoff || !isAlive(slot.fd)) {
                    closeFd(slot.fd);
                    metEvicted_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    survivors.push(slot);
                }
            }
            bucket->slots = std::move(survivors);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// runMetricsReporter
// ─────────────────────────────────────────────────────────────────────────────

void ConnectionPool::runMetricsReporter() noexcept
{
    while (metricsRunning_.load()) {
        {
            std::unique_lock<std::mutex> lock(metricsMutex_);
            metricsCv_.wait_for(lock,
                std::chrono::milliseconds(cfg_.metricsIntervalMs),
                [this]() { return !metricsRunning_.load(); });
        }
        if (!metricsRunning_.load()) return;

        Metrics snap = getMetrics();
        std::lock_guard<std::mutex> lock(metricsPushMutex_);
        if (metricsPushFn_) {
            try { metricsPushFn_(snap); }
            catch (const std::exception &e) {
                std::cerr << "[ConnectionPool] metrics push threw: "
                          << e.what() << "\n";
            } catch (...) {}
        }
    }
}
