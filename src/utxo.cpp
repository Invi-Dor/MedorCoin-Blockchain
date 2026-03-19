#include "utxo.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <sstream>

// =============================================================================
// CONSTRUCTOR
// =============================================================================
UTXOSet::UTXOSet(PersistFn fn) noexcept
    : persistFn_(std::move(fn))
{
    // Pre-reserve for large-scale deployment — avoids rehash at startup
    utxos_.reserve(1 << 20);        // 1M initial buckets
    addressIndex_.reserve(1 << 16); // 64K address buckets
}

// =============================================================================
// MAKE KEY
// Format: "<hashLen>:<txHash>:<index>"
// Length prefix guarantees uniqueness even when txHash contains ':'
// =============================================================================
std::string UTXOSet::makeKey(const std::string& txHash,
                               int outputIndex) noexcept
{
    return std::to_string(txHash.size())
         + ":" + txHash
         + ":" + std::to_string(outputIndex);
}

// =============================================================================
// LOGGER
// Thread-safe — uses logMu_ to protect logFn_ reads.
// Exceptions from user logFn_ are caught and never propagate.
// =============================================================================
void UTXOSet::setLogger(LogFn fn) noexcept {
    std::lock_guard<std::mutex> lk(logMu_);
    logFn_ = std::move(fn);
}

void UTXOSet::slog(int level, const std::string& msg) const noexcept {
    std::lock_guard<std::mutex> lk(logMu_);
    if (logFn_) {
        try { logFn_(level, "[UTXOSet] " + msg); }
        catch (const std::exception& e) {
            std::cerr << "[UTXOSet] logFn_ threw: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[UTXOSet] logFn_ threw unknown\n";
        }
        return;
    }
    if (level >= 1)
        std::cerr << "[UTXOSet] " << msg << "\n";
}

// =============================================================================
// SECONDARY INDEX HELPERS
// Precondition: write lock held by caller.
// =============================================================================
void UTXOSet::indexAdd(const std::string& address,
                        const std::string& key) noexcept
{
    addressIndex_[address].push_back(key);
}

void UTXOSet::indexRemove(const std::string& address,
                           const std::string& key) noexcept
{
    auto it = addressIndex_.find(address);
    if (it == addressIndex_.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), key), vec.end());
    if (vec.empty()) addressIndex_.erase(it);
}

// =============================================================================
// PERSIST
// Called while write lock is held — snapshot delivered to callback
// is guaranteed consistent with current in-memory state.
// Enqueues to background persist worker if configured for async mode.
// =============================================================================
void UTXOSet::persist() const noexcept {
    if (!persistFn_) return;

    if (cfg_.asyncPersist) {
        // Enqueue a snapshot copy to the background worker
        // Background worker calls persistFn_ outside the write lock
        auto snapshot = utxos_;
        {
            std::lock_guard<std::mutex> lk(persistQueueMu_);
            if (persistQueue_.size() < cfg_.maxPersistQueueDepth)
                persistQueue_.push_back(std::move(snapshot));
            else
                slog(1, "persist queue full — snapshot dropped");
        }
        persistQueueCv_.notify_one();
        return;
    }

    // Synchronous persist — called while write lock held
    try { persistFn_(utxos_); }
    catch (const std::exception& e) {
        slog(2, "persist callback threw: " + std::string(e.what()));
    } catch (...) {
        slog(2, "persist callback threw unknown exception");
    }
}

// =============================================================================
// BACKGROUND PERSIST WORKER
// Drains snapshot queue and calls persistFn_ outside any write lock.
// This prevents the write lock from being held during slow disk I/O.
// =============================================================================
void UTXOSet::runPersistWorker() noexcept {
    while (true) {
        std::unordered_map<std::string, UTXO> snapshot;
        {
            std::unique_lock<std::mutex> lk(persistQueueMu_);
            persistQueueCv_.wait(lk, [this]() {
                return !persistQueue_.empty()
                    || persistWorkerStopped_.load();
            });
            if (persistWorkerStopped_.load() && persistQueue_.empty()) break;
            snapshot = std::move(persistQueue_.back());
            persistQueue_.clear(); // Only the latest snapshot matters
        }
        if (!persistFn_) continue;
        try { persistFn_(snapshot); }
        catch (const std::exception& e) {
            slog(2, "async persist threw: " + std::string(e.what()));
        } catch (...) {
            slog(2, "async persist threw unknown");
        }
    }
}

void UTXOSet::startPersistWorker() noexcept {
    if (!cfg_.asyncPersist) return;
    persistWorkerStopped_.store(false);
    persistWorkerThread_ = std::thread([this]() { runPersistWorker(); });
}

void UTXOSet::stopPersistWorker() noexcept {
    if (!cfg_.asyncPersist) return;
    {
        std::lock_guard<std::mutex> lk(persistQueueMu_);
        persistWorkerStopped_.store(true);
    }
    persistQueueCv_.notify_all();
    if (persistWorkerThread_.joinable())
        persistWorkerThread_.join();
}

// =============================================================================
// ADD UTXO
// =============================================================================
bool UTXOSet::addUTXO(const TxOutput&    output,
                       const std::string& txHash,
                       int                outputIndex,
                       uint64_t           blockHeight,
                       bool               isCoinbase) noexcept
{
    if (txHash.empty()) {
        slog(2, "addUTXO: empty txHash rejected");
        return false;
    }
    if (outputIndex < 0) {
        slog(2, "addUTXO: negative outputIndex rejected");
        return false;
    }
    if (output.address.empty()) {
        slog(2, "addUTXO: empty address rejected");
        return false;
    }

    const std::string key = makeKey(txHash, outputIndex);

    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    if (utxos_.count(key)) {
        slog(1, "addUTXO: duplicate key '" + key + "' rejected");
        return false;
    }

    UTXO utxo;
    utxo.txHash      = txHash;
    utxo.outputIndex = outputIndex;
    utxo.value       = output.value;
    utxo.address     = output.address;
    utxo.blockHeight = blockHeight;
    utxo.isCoinbase  = isCoinbase;

    utxos_.emplace(key, utxo);
    indexAdd(output.address, key);

    metrics_.utxosAdded.fetch_add(1, std::memory_order_relaxed);
    metrics_.totalValueTracked.fetch_add(output.value,
        std::memory_order_relaxed);

    persist(); // called while write lock is held
    return true;
}

// =============================================================================
// SPEND UTXO
// =============================================================================
bool UTXOSet::spendUTXO(const std::string& txHash,
                          int                outputIndex,
                          uint64_t           currentBlockHeight) noexcept
{
    if (txHash.empty()) {
        slog(2, "spendUTXO: empty txHash rejected");
        return false;
    }
    if (outputIndex < 0) {
        slog(2, "spendUTXO: negative outputIndex rejected");
        return false;
    }

    const std::string key = makeKey(txHash, outputIndex);

    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    auto it = utxos_.find(key);
    if (it == utxos_.end()) {
        slog(1, "spendUTXO: key '" + key
                + "' not found (already spent or nonexistent)");
        return false;
    }

    const UTXO& utxo = it->second;

    // Coinbase maturity enforcement
    // UINT64_MAX is the rollback sentinel — bypasses maturity check
    if (utxo.isCoinbase &&
        currentBlockHeight != std::numeric_limits<uint64_t>::max())
    {
        if (currentBlockHeight < utxo.blockHeight + COINBASE_MATURITY) {
            slog(1, "spendUTXO: coinbase not matured key='" + key
                    + "' created=" + std::to_string(utxo.blockHeight)
                    + " current=" + std::to_string(currentBlockHeight)
                    + " required="
                    + std::to_string(utxo.blockHeight + COINBASE_MATURITY));
            metrics_.coinbaseMaturityRejected.fetch_add(
                1, std::memory_order_relaxed);
            return false;
        }
    }

    metrics_.totalValueTracked.fetch_sub(utxo.value,
        std::memory_order_relaxed);
    metrics_.utxosSpent.fetch_add(1, std::memory_order_relaxed);

    indexRemove(utxo.address, key);
    utxos_.erase(it);

    persist(); // called while write lock is held
    return true;
}

// =============================================================================
// GET BALANCE
// Returns nullopt on overflow — caller decides how to handle.
// =============================================================================
std::optional<uint64_t> UTXOSet::getBalance(
    const std::string& address) const noexcept
{
    if (address.empty()) return 0ULL;

    std::shared_lock<std::shared_mutex> lock(rwMutex_);

    const auto idxIt = addressIndex_.find(address);
    if (idxIt == addressIndex_.end()) return 0ULL;

    uint64_t balance = 0;
    for (const auto& key : idxIt->second) {
        const auto utxoIt = utxos_.find(key);
        if (utxoIt == utxos_.end()) continue;
        const uint64_t v = utxoIt->second.value;
        if (v > std::numeric_limits<uint64_t>::max() - balance) {
            slog(2, "getBalance: uint64 overflow for address '" + address + "'");
            return std::nullopt;
        }
        balance += v;
    }
    return balance;
}

// =============================================================================
// GET UTXOs FOR ADDRESS
// =============================================================================
std::vector<UTXO> UTXOSet::getUTXOsForAddress(
    const std::string& address) const noexcept
{
    std::vector<UTXO> result;
    if (address.empty()) return result;

    std::shared_lock<std::shared_mutex> lock(rwMutex_);

    const auto idxIt = addressIndex_.find(address);
    if (idxIt == addressIndex_.end()) return result;

    result.reserve(idxIt->second.size());
    for (const auto& key : idxIt->second) {
        const auto utxoIt = utxos_.find(key);
        if (utxoIt != utxos_.end())
            result.push_back(utxoIt->second);
    }
    return result;
}

// =============================================================================
// GET UTXO
// =============================================================================
std::optional<UTXO> UTXOSet::getUTXO(const std::string& txHash,
                                        int outputIndex) const noexcept
{
    if (txHash.empty() || outputIndex < 0) return std::nullopt;

    std::shared_lock<std::shared_mutex> lock(rwMutex_);

    const auto it = utxos_.find(makeKey(txHash, outputIndex));
    if (it == utxos_.end()) return std::nullopt;
    return it->second;
}

// =============================================================================
// IS UNSPENT
// =============================================================================
bool UTXOSet::isUnspent(const std::string& txHash,
                          int outputIndex) const noexcept
{
    if (txHash.empty() || outputIndex < 0) return false;

    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    return utxos_.count(makeKey(txHash, outputIndex)) > 0;
}

// =============================================================================
// SIZE
// =============================================================================
size_t UTXOSet::size() const noexcept {
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    return utxos_.size();
}

// =============================================================================
// CLEAR
// =============================================================================
void UTXOSet::clear() noexcept {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    utxos_.clear();
    addressIndex_.clear();
    metrics_.utxosAdded.store(0, std::memory_order_relaxed);
    metrics_.utxosSpent.store(0, std::memory_order_relaxed);
    metrics_.totalValueTracked.store(0, std::memory_order_relaxed);
    persist();
}

// =============================================================================
// LOAD SNAPSHOT
// Full validation pass before any live state is modified.
// Key consistency verified against freshly derived keys.
// =============================================================================
bool UTXOSet::loadSnapshot(
    const std::unordered_map<std::string, UTXO>& snapshot) noexcept
{
    // Validation pass — no lock needed (read-only)
    for (const auto& [storedKey, utxo] : snapshot) {
        if (utxo.txHash.empty()) {
            slog(2, "loadSnapshot: empty txHash — rejected");
            return false;
        }
        if (utxo.outputIndex < 0) {
            slog(2, "loadSnapshot: negative outputIndex — rejected");
            return false;
        }
        if (utxo.address.empty()) {
            slog(2, "loadSnapshot: empty address — rejected");
            return false;
        }
        const std::string derived = makeKey(utxo.txHash, utxo.outputIndex);
        if (storedKey != derived) {
            slog(2, "loadSnapshot: key mismatch stored='"
                    + storedKey + "' derived='" + derived + "' — rejected");
            return false;
        }
    }

    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    utxos_        = snapshot;
    addressIndex_.clear();

    uint64_t totalValue = 0;
    for (const auto& [key, utxo] : utxos_) {
        addressIndex_[utxo.address].push_back(key);
        if (utxo.value <= std::numeric_limits<uint64_t>::max() - totalValue)
            totalValue += utxo.value;
    }
    metrics_.totalValueTracked.store(totalValue, std::memory_order_relaxed);
    metrics_.utxosAdded.store(
        static_cast<uint64_t>(utxos_.size()), std::memory_order_relaxed);

    persist();
    return true;
}

// =============================================================================
// METRICS
// =============================================================================
UTXOSet::Metrics UTXOSet::getMetrics() const noexcept {
    Metrics m;
    m.utxosAdded              = metrics_.utxosAdded.load(
                                    std::memory_order_relaxed);
    m.utxosSpent              = metrics_.utxosSpent.load(
                                    std::memory_order_relaxed);
    m.totalValueTracked       = metrics_.totalValueTracked.load(
                                    std::memory_order_relaxed);
    m.coinbaseMaturityRejected= metrics_.coinbaseMaturityRejected.load(
                                    std::memory_order_relaxed);
    {
        std::shared_lock<std::shared_mutex> lock(rwMutex_);
        m.currentUtxoCount    = utxos_.size();
        m.currentAddressCount = addressIndex_.size();
    }
    return m;
}

std::string UTXOSet::getPrometheusText() const noexcept {
    auto m = getMetrics();
    std::ostringstream ss;
    ss << "# HELP utxo_added_total Total UTXOs added\n"
       << "# TYPE utxo_added_total counter\n"
       << "utxo_added_total " << m.utxosAdded << "\n"
       << "# HELP utxo_spent_total Total UTXOs spent\n"
       << "# TYPE utxo_spent_total counter\n"
       << "utxo_spent_total " << m.utxosSpent << "\n"
       << "# HELP utxo_current_count Current UTXO count\n"
       << "# TYPE utxo_current_count gauge\n"
       << "utxo_current_count " << m.currentUtxoCount << "\n"
       << "# HELP utxo_total_value_tracked Total value tracked\n"
       << "# TYPE utxo_total_value_tracked gauge\n"
       << "utxo_total_value_tracked " << m.totalValueTracked << "\n"
       << "# HELP utxo_address_count Unique addresses tracked\n"
       << "# TYPE utxo_address_count gauge\n"
       << "utxo_address_count " << m.currentAddressCount << "\n"
       << "# HELP utxo_coinbase_maturity_rejected Coinbase maturity rejections\n"
       << "# TYPE utxo_coinbase_maturity_rejected counter\n"
       << "utxo_coinbase_maturity_rejected "
       << m.coinbaseMaturityRejected << "\n";
    return ss.str();
}
