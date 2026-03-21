#include "mempool/mempool.h"

#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>

// =============================================================================
// TIME HELPER
// =============================================================================
static uint64_t nowSecs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now()
                .time_since_epoch()).count());
}

// =============================================================================
// CONSTRUCTOR
// =============================================================================
Mempool::Mempool(Config cfg, const UTXOSet& utxoSet)
    : cfg_(std::move(cfg))
    , utxoSet_(utxoSet)
    , persistStopped_(false)
{
    if (cfg_.persistToDisk && !cfg_.dbPath.empty()) {
        rocksdb::Options opts;
        opts.create_if_missing        = true;
        opts.paranoid_checks          = true;
        opts.compression              = rocksdb::kLZ4Compression;
        opts.write_buffer_size        = 64 * 1024 * 1024;
        opts.max_open_files           = 512;
        opts.max_background_jobs      = 4;
        opts.bytes_per_sync           = 1048576;
        opts.rate_limiter.reset(
            rocksdb::NewGenericRateLimiter(
                static_cast<int64_t>(cfg_.persistWriteBytesPerSec)));

        rocksdb::DB* raw = nullptr;
        auto s = rocksdb::DB::Open(opts, cfg_.dbPath, &raw);
        if (s.ok()) {
            db_.reset(raw);
            loadFromDisk();
            slog(0, "DB opened at " + cfg_.dbPath);
        } else {
            slog(1, "DB unavailable: " + s.ToString()
                    + " — in-memory only");
        }

        persistThread_ = std::thread([this]() { runPersistWorker(); });
    }
}

// =============================================================================
// DESTRUCTOR
// =============================================================================
Mempool::~Mempool() {
    {
        std::lock_guard<std::mutex> lk(persistQueueMu_);
        persistStopped_.store(true);
    }
    persistQueueCv_.notify_all();
    if (persistThread_.joinable()) persistThread_.join();
}

// =============================================================================
// CALLBACKS
// =============================================================================
void Mempool::setLogger(LogFn fn) {
    std::lock_guard<std::mutex> lk(logMu_);
    logFn_ = std::move(fn);
}

void Mempool::onEviction(EvictFn fn) {
    std::lock_guard<std::mutex> lk(evictMu_);
    evictFn_ = std::move(fn);
}

void Mempool::slog(int level, const std::string& msg) const noexcept {
    std::lock_guard<std::mutex> lk(logMu_);
    if (logFn_) {
        try { logFn_(level, "[Mempool] " + msg); }
        catch (...) {}
        return;
    }
    if (level >= 2) std::cerr << "[Mempool] " << msg << "\n";
}

// =============================================================================
// ESTIMATE TX BYTES
// =============================================================================
size_t Mempool::estimateTxBytes(const Transaction& tx) noexcept {
    size_t s = 0;
    s += sizeof(tx.chainId);
    s += sizeof(tx.nonce);
    s += sizeof(tx.maxPriorityFeePerGas);
    s += sizeof(tx.maxFeePerGas);
    s += sizeof(tx.gasLimit);
    s += sizeof(tx.value);
    s += sizeof(tx.v);
    s += 4 + tx.toAddress.size();
    s += 4 + tx.txHash.size();
    s += 4 + tx.data.size();
    s += tx.r.size();
    s += tx.s.size();
    s += 4;
    for (const auto& in : tx.inputs)
        s += 4 + in.prevTxHash.size() + sizeof(in.outputIndex);
    s += 4;
    for (const auto& out : tx.outputs)
        s += 4 + out.address.size() + sizeof(out.value);
    s += 64;
    return s;
}

// =============================================================================
// EFFECTIVE FEE
// =============================================================================
uint64_t Mempool::effectiveFee(const Transaction& tx,
                                 uint64_t baseFee) noexcept {
    uint64_t tip  = tx.maxPriorityFeePerGas;
    uint64_t maxF = tx.maxFeePerGas;
    uint64_t sum  = (tip > std::numeric_limits<uint64_t>::max() - baseFee)
                    ? std::numeric_limits<uint64_t>::max()
                    : baseFee + tip;
    return std::min(maxF, sum);
}

// =============================================================================
// VALIDATION
// =============================================================================
bool Mempool::validateTx(const Transaction& tx,
                           uint64_t baseFee) const noexcept {
    if (tx.txHash.empty() || tx.txHash.size() != 64) return false;
    if (!tx.isValid())                                 return false;
    if (tx.toAddress.empty())                          return false;
    if (effectiveFee(tx, baseFee) < cfg_.minFeePerGas) return false;

    if (!tx.inputs.empty()) {
        uint64_t inSum  = 0;
        uint64_t outSum = 0;
        for (const auto& in : tx.inputs) {
            auto utxo = utxoSet_.getUTXO(in.prevTxHash, in.outputIndex);
            if (!utxo) return false;
            if (utxo->value > std::numeric_limits<uint64_t>::max() - inSum)
                return false;
            inSum += utxo->value;
        }
        for (const auto& out : tx.outputs) {
            if (out.value > std::numeric_limits<uint64_t>::max() - outSum)
                return false;
            outSum += out.value;
        }
        if (outSum > inSum) return false;
    }
    return true;
}

bool Mempool::isDoubleSpend(const Transaction& tx) const noexcept {
    for (const auto& in : tx.inputs) {
        std::string key = in.prevTxHash + ":"
                        + std::to_string(in.outputIndex);
        if (spentOutpoints_.count(key)) return true;
    }
    return false;
}

bool Mempool::checkNonceOrdering(const Transaction& tx) const noexcept {
    uint64_t maxNonce = 0;
    bool     found    = false;
    for (const auto& [hash, entry] : entries_) {
        if (entry.tx.toAddress == tx.toAddress) {
            if (!found || entry.tx.nonce > maxNonce) {
                maxNonce = entry.tx.nonce;
                found    = true;
            }
        }
    }
    if (found && tx.nonce <= maxNonce) return false;
    return true;
}

// =============================================================================
// STRIPE LOCK
// =============================================================================
std::mutex& Mempool::stripeFor(const std::string& txHash) noexcept {
    size_t idx = std::hash<std::string>{}(txHash) % STRIPE_COUNT;
    return stripes_[idx];
}

// =============================================================================
// ADD TRANSACTION
// =============================================================================
bool Mempool::addTransaction(const Transaction& tx, uint64_t baseFee) {
    if (!validateTx(tx, baseFee)) {
        metRejected.fetch_add(1, std::memory_order_relaxed);
        slog(1, "rejected invalid tx: " + tx.txHash);
        return false;
    }

    size_t txBytes = estimateTxBytes(tx);

    uint64_t curBytes = totalBytes_.load(std::memory_order_relaxed);
    if (txBytes > std::numeric_limits<uint64_t>::max() - curBytes) {
        metRejected.fetch_add(1, std::memory_order_relaxed);
        slog(1, "totalBytes overflow — rejected: " + tx.txHash);
        return false;
    }

    std::lock_guard<std::mutex> stripe(stripeFor(tx.txHash));

    {
        std::unique_lock<std::shared_mutex> lk(mu_);

        if (entries_.count(tx.txHash)) {
            metRejected.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (isDoubleSpend(tx)) {
            metDsRejected.fetch_add(1, std::memory_order_relaxed);
            slog(1, "double-spend rejected: " + tx.txHash);
            return false;
        }
        if (!checkNonceOrdering(tx)) {
            addOrphanLocked(tx);
            return false;
        }

        bool overCount = entries_.size() >= cfg_.maxTxCount;
        bool overBytes = totalBytes_.load(std::memory_order_relaxed)
                         + txBytes > cfg_.maxTotalBytes;

        if (overCount || overBytes) {
            lk.unlock();
            trimToCapacity(baseFee);
            lk.lock();

            if (entries_.count(tx.txHash)) {
                metRejected.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (isDoubleSpend(tx)) {
                metDsRejected.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (entries_.size() >= cfg_.maxTxCount ||
                totalBytes_.load(std::memory_order_relaxed)
                    + txBytes > cfg_.maxTotalBytes) {
                metRejected.fetch_add(1, std::memory_order_relaxed);
                slog(1, "mempool full, rejected: " + tx.txHash);
                return false;
            }
        }

        MempoolEntry entry;
        entry.tx           = tx;
        entry.addedAt      = nowSecs();
        entry.effectiveFee = effectiveFee(tx, baseFee);
        entry.txBytes      = txBytes;

        for (const auto& in : tx.inputs)
            spentOutpoints_.insert(
                in.prevTxHash + ":" + std::to_string(in.outputIndex));

        entries_.emplace(tx.txHash, std::move(entry));
        totalBytes_.fetch_add(txBytes, std::memory_order_relaxed);
        metAdded.fetch_add(1, std::memory_order_relaxed);
    }

    enqueuePersist(tx);
    promoteOrphans(tx.txHash, baseFee);

    slog(2, "added tx: " + tx.txHash
            + " fee=" + std::to_string(effectiveFee(tx, baseFee)));
    return true;
}

// =============================================================================
// REMOVE TRANSACTION
// =============================================================================
void Mempool::removeTransaction(const std::string& txHash) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = entries_.find(txHash);
    if (it == entries_.end()) return;

    for (const auto& in : it->second.tx.inputs)
        spentOutpoints_.erase(
            in.prevTxHash + ":" + std::to_string(in.outputIndex));

    size_t bytes = it->second.txBytes;
    entries_.erase(it);
    lk.unlock();

    totalBytes_.fetch_sub(bytes, std::memory_order_relaxed);
    enqueueUnpersist(txHash);

    std::lock_guard<std::mutex> elk(evictMu_);
    if (evictFn_) {
        try { evictFn_(txHash); } catch (...) {}
    }
}

void Mempool::removeConfirmed(const std::vector<Transaction>& block) {
    for (const auto& tx : block) {
        removeTransaction(tx.txHash);
        metConfirmed.fetch_add(1, std::memory_order_relaxed);
    }
}

void Mempool::reorgRestore(const std::vector<Transaction>& reorgedOut) {
    for (const auto& tx : reorgedOut) {
        {
            std::shared_lock<std::shared_mutex> rlock(mu_);
            if (entries_.count(tx.txHash)) continue;
        }
        addTransaction(tx, cfg_.minFeePerGas);
    }
}

// =============================================================================
// QUERIES
// =============================================================================
bool Mempool::hasTransaction(const std::string& txHash) const noexcept {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return entries_.count(txHash) > 0;
}

std::optional<Transaction> Mempool::getTransaction(
    const std::string& txHash) const noexcept
{
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = entries_.find(txHash);
    if (it == entries_.end()) return std::nullopt;
    return it->second.tx;
}

std::vector<Transaction> Mempool::getTransactions() const noexcept {
    std::shared_lock<std::shared_mutex> lk(mu_);
    std::vector<Transaction> result;
    result.reserve(entries_.size());
    for (const auto& [h, e] : entries_) result.push_back(e.tx);
    return result;
}

// =============================================================================
// GET PENDING TXS
// Alias for getTransactions() -- called by miner.cpp
// =============================================================================
std::vector<Transaction> Mempool::getPendingTxs() const noexcept {
    return getTransactions();
}

std::vector<Transaction> Mempool::getSortedByFee(
    size_t maxCount, uint64_t baseFee) const noexcept
{
    std::shared_lock<std::shared_mutex> lk(mu_);
    std::vector<const MempoolEntry*> ptrs;
    ptrs.reserve(entries_.size());
    for (const auto& [h, e] : entries_) ptrs.push_back(&e);

    std::sort(ptrs.begin(), ptrs.end(),
        [&](const MempoolEntry* a, const MempoolEntry* b) {
            return effectiveFee(a->tx, baseFee)
                 > effectiveFee(b->tx, baseFee);
        });

    std::vector<Transaction> result;
    size_t n = std::min(maxCount, ptrs.size());
    result.reserve(n);
    for (size_t i = 0; i < n; i++)
        result.push_back(ptrs[i]->tx);
    return result;
}

size_t Mempool::txCount() const noexcept {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return entries_.size();
}

size_t Mempool::totalBytes() const noexcept {
    return static_cast<size_t>(
        totalBytes_.load(std::memory_order_relaxed));
}

uint64_t Mempool::minFee() const noexcept { return cfg_.minFeePerGas; }

// =============================================================================
// MAINTENANCE
// =============================================================================
void Mempool::evictStale(uint64_t) noexcept {
    const uint64_t cutoff = nowSecs() - cfg_.maxTxAgeSeconds;
    std::vector<std::string> toRemove;
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        for (const auto& [h, e] : entries_)
            if (e.addedAt < cutoff) toRemove.push_back(h);
    }
    for (const auto& h : toRemove) {
        removeTransaction(h);
        metEvicted.fetch_add(1, std::memory_order_relaxed);
    }
    evictStaleOrphans();
}

void Mempool::evictLowFee(uint64_t newBaseFee) noexcept {
    std::vector<std::string> toRemove;
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        for (const auto& [h, e] : entries_)
            if (effectiveFee(e.tx, newBaseFee) < cfg_.minFeePerGas)
                toRemove.push_back(h);
    }
    for (const auto& h : toRemove) {
        removeTransaction(h);
        metEvicted.fetch_add(1, std::memory_order_relaxed);
    }
}

void Mempool::trimToCapacity(uint64_t baseFee) noexcept {
    std::vector<std::pair<uint64_t, std::string>> ranked;
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        if (entries_.size() <= cfg_.maxTxCount &&
            totalBytes_.load(std::memory_order_relaxed) <= cfg_.maxTotalBytes)
            return;
        ranked.reserve(entries_.size());
        for (const auto& [h, e] : entries_)
            ranked.emplace_back(effectiveFee(e.tx, baseFee), h);
    }

    std::sort(ranked.begin(), ranked.end());

    for (const auto& [fee, hash] : ranked) {
        {
            std::shared_lock<std::shared_mutex> lk(mu_);
            if (entries_.size() <= cfg_.maxTxCount &&
                totalBytes_.load(std::memory_order_relaxed)
                    <= cfg_.maxTotalBytes)
                break;
        }
        removeTransaction(hash);
        metEvicted.fetch_add(1, std::memory_order_relaxed);
    }
}

// =============================================================================
// ORPHAN HANDLING
// =============================================================================
void Mempool::addOrphanLocked(const Transaction& tx) noexcept {
    if (orphans_.size() >= cfg_.orphanMaxCount) {
        auto it = std::min_element(
            orphans_.begin(), orphans_.end(),
            [](const auto& a, const auto& b) {
                return a.second.addedAt < b.second.addedAt;
            });
        if (it != orphans_.end()) {
            metOrphansEvicted.fetch_add(1, std::memory_order_relaxed);
            orphans_.erase(it);
        }
    }
    MempoolEntry e;
    e.tx      = tx;
    e.addedAt = nowSecs();
    e.txBytes = estimateTxBytes(tx);
    orphans_.emplace(tx.txHash, std::move(e));
    metOrphansAdded.fetch_add(1, std::memory_order_relaxed);
}

void Mempool::promoteOrphans(const std::string& confirmedTxHash,
                               uint64_t baseFee) noexcept
{
    std::vector<Transaction> toPromote;
    {
        std::unique_lock<std::shared_mutex> lk(mu_);
        for (auto it = orphans_.begin(); it != orphans_.end(); ) {
            bool depends = false;
            for (const auto& in : it->second.tx.inputs)
                if (in.prevTxHash == confirmedTxHash) {
                    depends = true; break;
                }
            if (depends) {
                toPromote.push_back(it->second.tx);
                it = orphans_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& tx : toPromote)
        addTransaction(tx, baseFee);
}

void Mempool::evictStaleOrphans() noexcept {
    const uint64_t cutoff = nowSecs() - cfg_.orphanMaxAgeSecs;
    std::unique_lock<std::shared_mutex> lk(mu_);
    for (auto it = orphans_.begin(); it != orphans_.end(); ) {
        if (it->second.addedAt < cutoff) {
            metOrphansEvicted.fetch_add(1, std::memory_order_relaxed);
            it = orphans_.erase(it);
        } else {
            ++it;
        }
    }
}

// =============================================================================
// METRICS
// =============================================================================
Mempool::Metrics Mempool::getMetrics() const noexcept {
    return Metrics{
        metAdded.load(),
        metRejected.load(),
        metEvicted.load(),
        metConfirmed.load(),
        metDsRejected.load(),
        metOrphansAdded.load(),
        metOrphansEvicted.load()
    };
}

// =============================================================================
// SERIALIZE TX
// =============================================================================
std::string Mempool::serializeTx(const Transaction& tx) noexcept {
    try {
        std::ostringstream oss;
        oss << tx.txHash << "|"
            << tx.chainId << "|"
            << tx.nonce   << "|"
            << tx.toAddress << "|"
            << tx.value   << "|"
            << tx.gasLimit << "|"
            << tx.maxFeePerGas << "|"
            << tx.maxPriorityFeePerGas;
        return oss.str();
    } catch (...) {
        return {};
    }
}

// =============================================================================
// LOAD FROM DISK
// =============================================================================
void Mempool::loadFromDisk() noexcept {
    if (!db_) return;
    std::unique_ptr<rocksdb::Iterator> it(
        db_->NewIterator(rocksdb::ReadOptions()));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        // Minimal restore -- txHash only, full tx not stored
        slog(0, "loaded tx key: " + it->key().ToString());
    }
}

// =============================================================================
// ENQUEUE PERSIST / UNPERSIST
// =============================================================================
void Mempool::enqueuePersist(const Transaction& tx) noexcept {
    {
        std::lock_guard<std::mutex> lk(persistQueueMu_);
        persistQueue_.push_back(
            PersistJob{PersistOp::Put, tx.txHash, serializeTx(tx)});
        if (persistQueue_.size() > cfg_.maxPersistQueueSize)
            persistQueue_.pop_front();
    }
    persistQueueCv_.notify_one();
}

void Mempool::enqueueUnpersist(const std::string& txHash) noexcept {
    {
        std::lock_guard<std::mutex> lk(persistQueueMu_);
        persistQueue_.push_back(
            PersistJob{PersistOp::Delete, txHash, {}});
        if (persistQueue_.size() > cfg_.maxPersistQueueSize)
            persistQueue_.pop_front();
    }
    persistQueueCv_.notify_one();
}

// =============================================================================
// RUN PERSIST WORKER
// =============================================================================
void Mempool::runPersistWorker() noexcept {
    while (true) {
        std::vector<PersistJob> batch;
        {
            std::unique_lock<std::mutex> lk(persistQueueMu_);
            persistQueueCv_.wait(lk, [this]() {
                return !persistQueue_.empty()
                    || persistStopped_.load();
            });
            if (persistStopped_.load() && persistQueue_.empty()) break;
            size_t n = std::min(persistQueue_.size(),
                                cfg_.persistBatchSize);
            batch.reserve(n);
            for (size_t i = 0; i < n; i++) {
                batch.push_back(std::move(persistQueue_.front()));
                persistQueue_.pop_front();
            }
        }

        if (!db_ || batch.empty()) continue;

        rocksdb::WriteBatch wb;
        for (const auto& job : batch) {
            if (job.op == PersistOp::Put)
                wb.Put(job.key, job.value);
            else
                wb.Delete(job.key);
        }

        rocksdb::WriteOptions wo;
        wo.sync = true;
        rocksdb::Status s = db_->Write(wo, &wb);
        if (!s.ok()) {
            slog(2, "persist batch failed: " + s.ToString());
            std::lock_guard<std::mutex> lk(persistQueueMu_);
            for (auto it = batch.rbegin(); it != batch.rend(); ++it)
                persistQueue_.push_front(std::move(*it));
        }
    }
}
