#include “connection_pool.h”

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// ── Platform I/O multiplexer selection ────────────────────────────────────────
#if defined(**linux**)

# include <sys/epoll.h>

# define MEDOR_EPOLL  1

#elif defined(**APPLE**) || defined(**FreeBSD**) || defined(**OpenBSD**) || defined(**NetBSD**)

# include <sys/event.h>

# define MEDOR_KQUEUE 1

#else

# include <poll.h>

# define MEDOR_POLL   1

#endif

// ─────────────────────────────────────────────────────────────────────────────
// Internal logging macros
// Replace these with your preferred logging library (spdlog, log4cpp, etc.)
// ─────────────────────────────────────────────────────────────────────────────
#define CP_LOG_INFO(msg)  do { std::cerr << “[ConnectionPool][INFO]  “ << msg << “\n”; } while(0)
#define CP_LOG_WARN(msg)  do { std::cerr << “[ConnectionPool][WARN]  “ << msg << “\n”; } while(0)
#define CP_LOG_ERROR(msg) do { std::cerr << “[ConnectionPool][ERROR] “ << msg << “\n”; } while(0)
#define CP_LOG_DEBUG(msg) do { std::cerr << “[ConnectionPool][DEBUG] “ << msg << “\n”; } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

std::string ConnectionPool::makeKey(const std::string &h, uint16_t p) noexcept
{
return std::to_string(h.size()) + “:” + h + “:” + std::to_string(p);
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
if (::close(fd) != 0)
CP_LOG_WARN(“closeFd: close(” << fd << “) failed: “ << std::strerror(errno));
if (openFds_.load(std::memory_order_relaxed) > 0)
openFds_.fetch_sub(1, std::memory_order_relaxed);
}

// FIX: pp = p in capture list — was missing, causing ‘pp not declared’ error
void ConnectionPool::joinWithTimeout(std::thread &t, uint32_t ms) noexcept
{
if (!t.joinable()) return;
auto p = std::make_shared<std::promise<void>>();
auto f = p->get_future();
std::thread helper([th = std::move(t), pp = p]() mutable {
th.join();
pp->set_value();
});
helper.detach();
if (f.wait_for(std::chrono::milliseconds(ms)) == std::future_status::timeout)
CP_LOG_WARN(“joinWithTimeout: thread did not finish within “ << ms << “ms”);
}

// ─────────────────────────────────────────────────────────────────────────────
// applySocketOptions — full error reporting on every setsockopt
// ─────────────────────────────────────────────────────────────────────────────
void ConnectionPool::applySocketOptions(int fd) noexcept
{
int one = 1;
if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0)
CP_LOG_WARN(“setsockopt TCP_NODELAY fd=” << fd << “: “ << std::strerror(errno));
if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) != 0)
CP_LOG_WARN(“setsockopt SO_KEEPALIVE fd=” << fd << “: “ << std::strerror(errno));

#if defined(TCP_KEEPIDLE)
int keepIdle = 60;
if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(keepIdle)) != 0)
CP_LOG_WARN(“setsockopt TCP_KEEPIDLE fd=” << fd);
#elif defined(TCP_KEEPALIVE)
int keepAlive = 60;
if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &keepAlive, sizeof(keepAlive)) != 0)
CP_LOG_WARN(“setsockopt TCP_KEEPALIVE fd=” << fd);
#endif
#if defined(TCP_KEEPINTVL)
int keepIntvl = 10;
if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepIntvl, sizeof(keepIntvl)) != 0)
CP_LOG_WARN(“setsockopt TCP_KEEPINTVL fd=” << fd);
#endif
#if defined(TCP_KEEPCNT)
int keepCnt = 3;
if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepCnt, sizeof(keepCnt)) != 0)
CP_LOG_WARN(“setsockopt TCP_KEEPCNT fd=” << fd);
#endif
#if defined(SO_NOSIGPIPE)
if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0)
CP_LOG_WARN(“setsockopt SO_NOSIGPIPE fd=” << fd);
#endif
#if defined(TCP_USER_TIMEOUT) && defined(**linux**)
unsigned int userTimeout = 30000;
if (::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &userTimeout, sizeof(userTimeout)) != 0)
CP_LOG_WARN(“setsockopt TCP_USER_TIMEOUT fd=” << fd);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Reactor initialisation
// ─────────────────────────────────────────────────────────────────────────────
bool ConnectionPool::initReactor() noexcept
{
#if defined(MEDOR_EPOLL)
reactorFd_ = ::epoll_create1(EPOLL_CLOEXEC);
if (reactorFd_ < 0) {
CP_LOG_ERROR(“epoll_create1 failed: “ << std::strerror(errno));
return false;
}
#elif defined(MEDOR_KQUEUE)
reactorFd_ = ::kqueue();
if (reactorFd_ < 0) {
CP_LOG_ERROR(“kqueue failed: “ << std::strerror(errno));
return false;
}
#else
reactorFd_ = 0;
#endif
CP_LOG_INFO(“Reactor initialised fd=” << reactorFd_);
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
if (::epoll_ctl(reactorFd_, EPOLL_CTL_ADD, fd, &ev) != 0)
CP_LOG_WARN(“epoll_ctl ADD fd=” << fd << “: “ << std::strerror(errno));
#elif defined(MEDOR_KQUEUE)
struct kevent change{};
EV_SET(&change, fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, nullptr);
struct timespec zero{ 0, 0 };
if (::kevent(reactorFd_, &change, 1, nullptr, 0, &zero) != 0)
CP_LOG_WARN(“kevent ADD fd=” << fd << “: “ << std::strerror(errno));
#endif
}

void ConnectionPool::unregisterFd(int fd) noexcept
{
#if defined(MEDOR_EPOLL)
if (::epoll_ctl(reactorFd_, EPOLL_CTL_DEL, fd, nullptr) != 0 && errno != ENOENT)
CP_LOG_WARN(“epoll_ctl DEL fd=” << fd << “: “ << std::strerror(errno));
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
if (it == pendingConnects_.end()) {
CP_LOG_WARN(“handleReactorEvent: fd=” << fd << “ not in pending map”);
return;
}
pc = std::move(it->second);
pendingConnects_.erase(it);
}
unregisterFd(fd);

```
auto fireError = [&]() {
    closeFd(fd);
    metFailed_.fetch_add(1, std::memory_order_relaxed);
    if (pc.cb) try { pc.cb(-1); }
    catch (const std::exception &e) { CP_LOG_ERROR("AcquireCb threw: " << e.what()); }
    catch (...) { CP_LOG_ERROR("AcquireCb threw unknown"); }
};

if (error) {
    CP_LOG_WARN("handleReactorEvent: error event fd=" << fd);
    fireError(); return;
}

int err = 0; socklen_t el = sizeof(err);
if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) {
    CP_LOG_WARN("handleReactorEvent: SO_ERROR=" << err << " host=" << pc.host);
    fireError(); return;
}

int flags = ::fcntl(fd, F_GETFL, 0);
if (flags < 0) {
    CP_LOG_ERROR("fcntl F_GETFL failed fd=" << fd);
    fireError(); return;
}
if (::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0)
    CP_LOG_WARN("fcntl restore blocking failed fd=" << fd);

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
if (pc.cb) try { pc.cb(fd); }
catch (const std::exception &e) { CP_LOG_ERROR("AcquireCb threw: " << e.what()); closeFd(fd); }
catch (...) { CP_LOG_ERROR("AcquireCb threw unknown"); closeFd(fd); }
```

}

// ─────────────────────────────────────────────────────────────────────────────
// runReactor
// ─────────────────────────────────────────────────────────────────────────────
void ConnectionPool::runReactor() noexcept
{
CP_LOG_INFO(“Reactor thread started”);
while (reactorRunning_.load()) {
#if defined(MEDOR_EPOLL)
constexpr int MAX_EVENTS = 64;
struct epoll_event events[MAX_EVENTS];
int n = ::epoll_wait(reactorFd_, events, MAX_EVENTS, 50);
if (n < 0) { if (errno == EINTR) continue; CP_LOG_ERROR(“epoll_wait: “ << std::strerror(errno)); break; }
for (int i = 0; i < n; ++i)
handleReactorEvent(events[i].data.fd, (events[i].events & (EPOLLERR | EPOLLHUP)) != 0);

#elif defined(MEDOR_KQUEUE)
constexpr int MAX_EVENTS = 64;
struct kevent events[MAX_EVENTS];
struct timespec ts{ 0, 50’000’000L };
int n = ::kevent(reactorFd_, nullptr, 0, events, MAX_EVENTS, &ts);
if (n < 0) { if (errno == EINTR) continue; CP_LOG_ERROR(“kevent: “ << std::strerror(errno)); break; }
for (int i = 0; i < n; ++i)
handleReactorEvent(static_cast<int>(events[i].ident), (events[i].flags & EV_ERROR) != 0);

#else
std::vector<pollfd> pfds;
std::vector<int> fdList;
{
std::unique_lock<std::mutex> lock(pendingMutex_);
for (const auto &[fd, *] : pendingConnects*) {
pfds.push_back({ fd, POLLOUT, 0 });
fdList.push_back(fd);
}
}
if (pfds.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
int n = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 50);
if (n < 0) { if (errno == EINTR) continue; CP_LOG_ERROR(“poll: “ << std::strerror(errno)); break; }
for (size_t i = 0; i < pfds.size(); ++i) {
if (pfds[i].revents == 0) continue;
handleReactorEvent(fdList[i], (pfds[i].revents & (POLLERR | POLLHUP)) != 0);
}
#endif
// Connect timeout sweep
const uint64_t now = nowMs();
std::vector<int> timedOut;
{
std::unique_lock<std::mutex> lock(pendingMutex_);
for (const auto &[fd, pc] : pendingConnects_)
if (pc.deadlineMs > 0 && now > pc.deadlineMs)
timedOut.push_back(fd);
}
for (int fd : timedOut) {
CP_LOG_WARN(“Reactor: connect timeout fd=” << fd);
handleReactorEvent(fd, true);
}
}
CP_LOG_INFO(“Reactor thread exiting”);
}

// ─────────────────────────────────────────────────────────────────────────────
// openAsync
// FIX: removed outer FdGuard guard; declaration — each loop iteration declares
// its own FdGuard, so no copy/assign of a non-copyable type occurs.
// ─────────────────────────────────────────────────────────────────────────────
void ConnectionPool::openAsync(const std::string &host,
uint16_t           port,
AcquireCallback    cb) noexcept
{
if (openFds_.load(std::memory_order_relaxed) >= cfg_.globalMaxFds) {
CP_LOG_WARN(“openAsync: global fd budget exhausted host=” << host);
metGlobalRej_.fetch_add(1, std::memory_order_relaxed);
if (cb) try { cb(-1); } catch (…) {}
return;
}

```
struct addrinfo hints{}, *res = nullptr;
hints.ai_family   = AF_UNSPEC;
hints.ai_socktype = SOCK_STREAM;
hints.ai_flags    = AI_NUMERICSERV | AI_ADDRCONFIG;

const std::string portStr = std::to_string(port);
int gai = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
if (gai != 0 || !res) {
    CP_LOG_ERROR("openAsync: getaddrinfo " << host << ":" << port
                 << " — " << ::gai_strerror(gai));
    metFailed_.fetch_add(1, std::memory_order_relaxed);
    if (cb) try { cb(-1); } catch (...) {}
    return;
}

bool registered = false;
for (struct addrinfo *ai = res; ai && !registered; ai = ai->ai_next) {
    FdGuard guard(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (guard.fd < 0) {
        CP_LOG_WARN("openAsync: socket() failed: " << std::strerror(errno));
        continue;
    }
    int flags = ::fcntl(guard.fd, F_GETFL, 0);
    if (flags < 0) continue;
    if (::fcntl(guard.fd, F_SETFL, flags | O_NONBLOCK) != 0) continue;

    int rc = ::connect(guard.fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
        CP_LOG_WARN("openAsync: connect() failed: " << std::strerror(errno));
        continue;
    }

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
    CP_LOG_ERROR("openAsync: all addresses failed for " << host << ":" << port);
    metFailed_.fetch_add(1, std::memory_order_relaxed);
    if (cb) try { cb(-1); } catch (...) {}
}
```

}

// ─────────────────────────────────────────────────────────────────────────────
// openBlocking
// ─────────────────────────────────────────────────────────────────────────────
int ConnectionPool::openBlocking(const std::string &host, uint16_t port) noexcept
{
if (openFds_.load(std::memory_order_relaxed) >= cfg_.globalMaxFds) {
CP_LOG_WARN(“openBlocking: global fd budget exhausted”);
metGlobalRej_.fetch_add(1, std::memory_order_relaxed);
return -1;
}

```
struct addrinfo hints{}, *res = nullptr;
hints.ai_family   = AF_UNSPEC;
hints.ai_socktype = SOCK_STREAM;
hints.ai_flags    = AI_NUMERICSERV | AI_ADDRCONFIG;

const std::string portStr = std::to_string(port);
uint32_t delayMs = cfg_.retryBaseDelayMs;

for (uint32_t attempt = 0; attempt <= cfg_.maxConnectRetries; ++attempt) {
    if (attempt > 0) {
        CP_LOG_INFO("openBlocking: retry " << attempt << " for "
                    << host << ":" << port << " delay=" << delayMs << "ms");
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        delayMs = std::min(delayMs * 2u, uint32_t{8000});
    }
    res = nullptr;
    int gai = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        CP_LOG_WARN("openBlocking: getaddrinfo attempt " << attempt
                    << ": " << ::gai_strerror(gai));
        continue;
    }
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        FdGuard guard(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (guard.fd < 0) continue;

        int flags = ::fcntl(guard.fd, F_GETFL, 0);
        if (flags < 0) continue;
        if (::fcntl(guard.fd, F_SETFL, flags | O_NONBLOCK) != 0) continue;

        int rc = ::connect(guard.fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) continue;

        uint32_t waited = 0;
        bool writable = false;
        while (waited < cfg_.connectTimeoutMs) {
            uint32_t slice = std::min(50u, cfg_.connectTimeoutMs - waited);
            fd_set wfds; FD_ZERO(&wfds); FD_SET(guard.fd, &wfds);
            struct timeval tv;
            tv.tv_sec  = slice / 1000;
            tv.tv_usec = (slice % 1000) * 1000;
            int sel = ::select(guard.fd + 1, nullptr, &wfds, nullptr, &tv);
            if (sel > 0) { writable = true; break; }
            if (sel < 0 && errno != EINTR) break;
            waited += slice;
        }
        if (!writable) {
            CP_LOG_WARN("openBlocking: connect timeout " << host << ":" << port);
            continue;
        }

        int err = 0; socklen_t el = sizeof(err);
        if (::getsockopt(guard.fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) {
            CP_LOG_WARN("openBlocking: SO_ERROR=" << err);
            continue;
        }

        if (::fcntl(guard.fd, F_SETFL, flags & ~O_NONBLOCK) != 0)
            CP_LOG_WARN("openBlocking: failed to restore blocking mode");

        applySocketOptions(guard.fd);

        int finalFd = applyTlsDirect(guard.fd, host, port);
        if (finalFd < 0) {
            CP_LOG_ERROR("openBlocking: TLS failed " << host << ":" << port);
            continue;
        }

        guard.release();
        ::freeaddrinfo(res);
        openFds_.fetch_add(1, std::memory_order_relaxed);
        CP_LOG_DEBUG("openBlocking: connected fd=" << finalFd
                     << " -> " << host << ":" << port);
        return finalFd;
    }
    ::freeaddrinfo(res);
}

CP_LOG_ERROR("openBlocking: all retries exhausted " << host << ":" << port);
metFailed_.fetch_add(1, std::memory_order_relaxed);
return -1;
```

}

// ─────────────────────────────────────────────────────────────────────────────
// applyTlsDirect — null-safe: returns raw fd if no TLS function registered
// ─────────────────────────────────────────────────────────────────────────────
int ConnectionPool::applyTlsDirect(int fd, const std::string &host, uint16_t port) noexcept
{
std::lock_guard<std::mutex> lock(tlsMutex_);
if (!tlsFn_) return fd;
int result = -1;
try {
result = tlsFn_(fd, host, port);
if (result < 0)
CP_LOG_ERROR(“applyTlsDirect: returned -1 for “ << host << “:” << port);
} catch (const std::exception &e) {
CP_LOG_ERROR(“applyTlsDirect: threw: “ << e.what());
} catch (…) {
CP_LOG_ERROR(“applyTlsDirect: threw unknown”);
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
ssize_t rc = ::send(fd, “”, 0, MSG_NOSIGNAL);
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
ConnectionPool::ConnectionPool(Config cfg) : cfg_(std::move(cfg))
{
CP_LOG_INFO(“ConnectionPool starting up”);
if (!initReactor()) {
CP_LOG_ERROR(“FATAL: reactor init failed”);
return;
}
reactorRunning_.store(true);
reactorThread_ = std::thread([this]() { runReactor(); });

```
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
CP_LOG_INFO("ConnectionPool ready");
```

}

ConnectionPool::~ConnectionPool()
{
CP_LOG_INFO(“ConnectionPool shutting down”);
reactorRunning_.store(false);
if (reactorFd_ >= 0) {
#if defined(MEDOR_EPOLL) || defined(MEDOR_KQUEUE)
::close(reactorFd_);
reactorFd_ = -1;
#endif
}
joinWithTimeout(reactorThread_, cfg_.shutdownTimeoutMs);

```
dnsRunning_.store(false);   dnsCv_.notify_all();
for (auto &t : dnsWorkers_) joinWithTimeout(t, cfg_.shutdownTimeoutMs);

tlsRunning_.store(false);   tlsCv_.notify_all();
for (auto &t : tlsWorkers_) joinWithTimeout(t, cfg_.shutdownTimeoutMs);

asyncRunning_.store(false); asyncCv_.notify_all();
for (auto &t : asyncWorkers_) joinWithTimeout(t, cfg_.shutdownTimeoutMs);

janitorRunning_.store(false); janitorCv_.notify_all();
for (auto &t : janitorWorkers_) joinWithTimeout(t, cfg_.shutdownTimeoutMs);

metricsRunning_.store(false); metricsCv_.notify_all();
joinWithTimeout(metricsThread_, cfg_.shutdownTimeoutMs);

clear();
CP_LOG_INFO("ConnectionPool destroyed");
```

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
Slot slot = bucket->slots.front(); bucket->slots.pop();
if (slot.lastUsed < cutoff || !isAlive(slot.fd)) {
CP_LOG_DEBUG(“acquire: evicting stale fd=” << slot.fd);
closeFd(slot.fd);
metEvicted_.fetch_add(1, std::memory_order_relaxed);
continue;
}
metAcquired_.fetch_add(1, std::memory_order_relaxed);
CP_LOG_DEBUG(“acquire: reusing fd=” << slot.fd << “ for “ << host << “:” << port);
return slot.fd;
}
}
int fd = openBlocking(host, port);
if (fd >= 0) metAcquired_.fetch_add(1, std::memory_order_relaxed);
return fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// acquireAsync
// FIX: lambda is (int) not () — matches AcquireCallback = std::function<void(int)>
// ─────────────────────────────────────────────────────────────────────────────
void ConnectionPool::acquireAsync(const std::string &host,
uint16_t           port,
AcquireCallback    cb) noexcept
{
if (!cb) {
CP_LOG_WARN(“acquireAsync: null callback ignored”);
return;
}

```
AcquireCallback cbCopy = cb;
const std::string k      = makeKey(host, port);
const uint64_t    cutoff = nowSecs() - cfg_.idleTimeoutSecs;
auto bucket              = getBucket(k);

{
    std::unique_lock<std::mutex> lock(bucket->mtx);
    while (!bucket->slots.empty()) {
        Slot slot = bucket->slots.front(); bucket->slots.pop();
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
            // FIX: (int) parameter matches AcquireCallback = void(int)
            req2.cb   = [pooledFd, cbCopy](int) { cbCopy(pooledFd); };
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
```

}

// ─────────────────────────────────────────────────────────────────────────────
// release
// ─────────────────────────────────────────────────────────────────────────────
void ConnectionPool::release(const std::string &host, uint16_t port,
int fd, bool healthy) noexcept
{
if (fd < 0) return;
if (!healthy) {
CP_LOG_DEBUG(“release: fd=” << fd << “ unhealthy — closing”);
closeFd(fd); return;
}
auto bucket = getBucket(makeKey(host, port));
{
std::unique_lock<std::mutex> lock(bucket->mtx);
if (bucket->slots.size() >= cfg_.maxConnsPerPeer) {
lock.unlock();
CP_LOG_DEBUG(“release: pool full for “ << host << “:” << port << “ — closing fd=” << fd);
closeFd(fd); return;
}
bucket->slots.push({ fd, nowSecs() });
}
metReleased_.fetch_add(1, std::memory_order_relaxed);
CP_LOG_DEBUG(“release: fd=” << fd << “ returned to pool “ << host << “:” << port);
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
CP_LOG_INFO(“evictPeer: all connections evicted for “ << host << “:” << port);
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
CP_LOG_INFO(“clear: all connections closed”);
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
CP_LOG_INFO(“DNS worker started”);
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
if (req.cb) try { req.cb(-1); } catch (…) {}
continue;
}
if (metAsyncDepth_.load(std::memory_order_relaxed) > 0)
metAsyncDepth_.fetch_sub(1, std::memory_order_relaxed);
openAsync(req.host, req.port, std::move(req.cb));
}
CP_LOG_INFO(“DNS worker exiting”);
}

// ─────────────────────────────────────────────────────────────────────────────
// runTlsWorker
// ─────────────────────────────────────────────────────────────────────────────
void ConnectionPool::runTlsWorker() noexcept
{
CP_LOG_INFO(“TLS worker started”);
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
CP_LOG_ERROR(“TLS worker: handshake failed “ << req.host << “:” << req.port);
closeFd(req.fd);
if (req.cb) try { req.cb(-1); } catch (…) {}
continue;
}
openFds_.fetch_add(1, std::memory_order_relaxed);
metAcquired_.fetch_add(1, std::memory_order_relaxed);
if (req.cb) try { req.cb(finalFd); }
catch (const std::exception &e) { CP_LOG_ERROR(“TLS cb threw: “ << e.what()); closeFd(finalFd); }
catch (…) { CP_LOG_ERROR(“TLS cb threw unknown”); closeFd(finalFd); }
}
CP_LOG_INFO(“TLS worker exiting”);
}

// ─────────────────────────────────────────────────────────────────────────────
// runAsyncWorker
// ─────────────────────────────────────────────────────────────────────────────
void ConnectionPool::runAsyncWorker() noexcept
{
CP_LOG_INFO(“Async worker started”);
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
CP_LOG_ERROR(“Async worker cb threw: “ << e.what());
if (fd >= 0) release(req.host, req.port, fd, false);
} catch (…) {
CP_LOG_ERROR(“Async worker cb threw unknown”);
if (fd >= 0) release(req.host, req.port, fd, false);
}
}
CP_LOG_INFO(“Async worker exiting”);
}

// ─────────────────────────────────────────────────────────────────────────────
// runJanitorWorker
// ─────────────────────────────────────────────────────────────────────────────
void ConnectionPool::runJanitorWorker(size_t /*idx*/) noexcept
{
CP_LOG_INFO(“Janitor worker started”);
while (janitorRunning_.load()) {
{
std::unique_lock<std::mutex> lock(janitorMutex_);
janitorCv_.wait_for(lock,
std::chrono::seconds(cfg_.janitorIntervalSecs),
[this]() { return !janitorRunning_.load(); });
}
if (!janitorRunning_.load()) return;

```
    const uint64_t cutoff = nowSecs() - cfg_.idleTimeoutSecs;
    std::vector<std::string> keys;
    {
        std::shared_lock<std::shared_mutex> rlock(mapMutex_);
        keys.reserve(pool_.size());
        for (const auto &[k, _] : pool_) keys.push_back(k);
    }
    uint64_t swept = 0;
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
                ++swept;
            } else {
                survivors.push(slot);
            }
        }
        bucket->slots = std::move(survivors);
    }
    if (swept > 0)
        CP_LOG_INFO("Janitor: swept " << swept << " idle connections");
}
CP_LOG_INFO("Janitor worker exiting");
```

}

// ─────────────────────────────────────────────────────────────────────────────
// runMetricsReporter
// ─────────────────────────────────────────────────────────────────────────────
void ConnectionPool::runMetricsReporter() noexcept
{
CP_LOG_INFO(“Metrics reporter started”);
while (metricsRunning_.load()) {
{
std::unique_lock<std::mutex> lock(metricsMutex_);
metricsCv_.wait_for(lock,
std::chrono::milliseconds(cfg_.metricsIntervalMs),
[this]() { return !metricsRunning_.load(); });
}
if (!metricsRunning_.load()) return;

```
    Metrics snap = getMetrics();
    std::lock_guard<std::mutex> lock(metricsPushMutex_);
    if (metricsPushFn_) {
        try { metricsPushFn_(snap); }
        catch (const std::exception &e) { CP_LOG_ERROR("Metrics cb threw: " << e.what()); }
        catch (...) { CP_LOG_ERROR("Metrics cb threw unknown"); }
    } else {
        CP_LOG_INFO("Metrics acquired=" << snap.acquired
                    << " released=" << snap.released
                    << " evicted=" << snap.evicted
                    << " failed=" << snap.failed
                    << " openFds=" << snap.currentOpenFds
                    << " tlsFail=" << snap.tlsFailures
                    << " asyncDepth=" << snap.asyncQueueDepth
                    << " reactorEvt=" << snap.reactorEvents);
    }
}
CP_LOG_INFO("Metrics reporter exiting");
```

}
