#pragma once

#include "transaction.h"
#include "utxo.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/rate_limiter.h>

// =============================================================================
// MEMPOOL ENTRY
// =============================================================================
struct MempoolEntry {
    Transaction tx;
    uint64_t    addedAt      = 0;
    uint64_t    effectiveFee = 0;
    size_t      txBytes      = 0;
};

// =============================================================================
// MEMPOOL
//
// Thread safety:
//   - mu_ (shared_mutex) protects entries_, orphans_, spentOutpoints_.
//   - Reads use shared_lock; writes use unique_lock.
//   - Fix 2: STRIPE_COUNT per-hash stripe mutexes reduce hot-path contention
//     under millions of concurrent addTransaction calls.
//   - Fix 1: all disk I/O runs on persistThread_ — addTransaction never
//     blocks on disk writes.
//   - Fix 4: WriteBatch with sync=true per batch — one fsync per batch
//     gives crash safety without per-transaction fsync cost.
//   - Fix 3: estimateTxBytes accounts for every variable-length field
//     including input/output arrays and signature bytes.
//
// Persistence:
//   - enqueuePersist / enqueueUnpersist post jobs to persistQueue_.
//   - runPersistWorker() drains queue in configurable batch sizes.
//   - On write failure, jobs are re-queued at front for retry.
//   - On shutdown, destructor signals worker and joins; final flush
//     drains remaining queue synchronously before db_ closes.
//
// Config:
//   - persistWriteBytesPerSec: RocksDB rate limiter for large-scale networks.
//   - persistBatchSize: max ops per WriteBatch flush.
//   - maxPersistQueueSize: max pending jobs before oldest is dropped.
// =============================================================================
class Mempool {
public:
    struct Config {
        size_t      maxTxCount             = 100000;
        uint64_t    maxTotalBytes          = 256ULL * 1024 * 1024;
        uint64_t    minFeePerGas           = 1;
        uint64_t    maxTxAgeSeconds        = 3600;
        bool        persistToDisk          = false;
        std::string dbPath;
        size_t      orphanMaxCount         = 1000;
        uint64_t    orphanMaxAgeSecs       = 300;
        // Fix 1: background persist tuning
        size_t      persistBatchSize       = 512;
        size_t      maxPersistQueueSize    = 50000;
        // Fix 4: RocksDB write rate limiter (bytes/sec, 0 = unlimited)
        int64_t     persistWriteBytesPerSec = 50 * 1024 * 1024; // 50 MB/s
    };

    struct Metrics {
        uint64_t added;
        uint64_t rejected;
        uint64_t evicted;
        uint64_t confirmed;
        uint64_t doubleSpendRejected;
        uint64_t orphansAdded;
        uint64_t orphansEvicted;
    };

    using LogFn   = std::function<void(int, const std::string&)>;
    using EvictFn = std::function<void(const std::string& txHash)>;

    explicit Mempool(Config cfg, const UTXOSet& utxoSet);
    ~Mempool();

    Mempool(const Mempool&)            = delete;
    Mempool& operator=(const Mempool&) = delete;

    void setLogger  (LogFn fn);
    void onEviction (EvictFn fn);

    // Core operations
    bool addTransaction   (const Transaction& tx, uint64_t baseFee);
    void removeTransaction(const std::string& txHash);
    void removeConfirmed  (const std::vector<Transaction>& block);
    void reorgRestore     (const std::vector<Transaction>& reorgedOut);

    // Queries
    bool                       hasTransaction (const std::string& txHash) const noexcept;
    std::optional<Transaction> getTransaction (const std::string& txHash) const noexcept;
    std::vector<Transaction>   getTransactions()                           const noexcept;
    std::vector<Transaction>   getSortedByFee (size_t maxCount,
                                                uint64_t baseFee)          const noexcept;
    size_t                     txCount()    const noexcept;
    size_t                     totalBytes() const noexcept;
    uint64_t                   minFee()     const noexcept;

    // Maintenance
    void evictStale     (uint64_t baseFee)    noexcept;
    void evictLowFee    (uint64_t newBaseFee) noexcept;
    void trimToCapacity (uint64_t baseFee)    noexcept;

    // Metrics
    Metrics getMetrics() const noexcept;

    // Static helpers — exposed for testing
    static size_t   estimateTxBytes(const Transaction& tx) noexcept;
    static uint64_t effectiveFee   (const Transaction& tx,
                                     uint64_t baseFee)      noexcept;

private:
    Config             cfg_;
    const UTXOSet&     utxoSet_;

    // Primary storage
    mutable std::shared_mutex                         mu_;
    std::unordered_map<std::string, MempoolEntry>     entries_;
    std::unordered_map<std::string, MempoolEntry>     orphans_;
    std::unordered_set<std::string>                   spentOutpoints_;
    std::atomic<uint64_t>                             totalBytes_{0};

    // Fix 2: stripe locks — reduce contention under high concurrency
    static constexpr size_t STRIPE_COUNT = 128;
    mutable std::array<std::mutex, STRIPE_COUNT> stripes_;
    std::mutex& stripeFor(const std::string& txHash) noexcept;

    // RocksDB
    std::unique_ptr<rocksdb::DB> db_;

    // Fix 1: background persist worker
    enum class PersistOp { Put, Delete };
    struct PersistJob {
        PersistOp   op;
        std::string key;
        std::string value;
    };
    mutable std::mutex              persistQueueMu_;
    std::condition_variable         persistQueueCv_;
    std::deque<PersistJob>          persistQueue_;
    std::atomic<bool>               persistStopped_;
    std::thread                     persistThread_;

    void enqueuePersist  (const Transaction& tx)     noexcept;
    void enqueueUnpersist(const std::string& txHash) noexcept;
    void runPersistWorker()                           noexcept;
    void loadFromDisk()                               noexcept;

    static std::string serializeTx(const Transaction& tx) noexcept;

    // Callbacks
    mutable std::mutex logMu_;
    mutable std::mutex evictMu_;
    LogFn              logFn_;
    EvictFn            evictFn_;

    void slog(int level, const std::string& msg) const noexcept;

    // Validation
    bool validateTx        (const Transaction& tx,
                             uint64_t baseFee)       const noexcept;
    bool isDoubleSpend     (const Transaction& tx)   const noexcept;
    bool checkNonceOrdering(const Transaction& tx)   const noexcept;

    // Orphan handling
    void addOrphanLocked  (const Transaction& tx)                   noexcept;
    void promoteOrphans   (const std::string& confirmedTxHash,
                            uint64_t baseFee)                        noexcept;
    void evictStaleOrphans()                                         noexcept;

    // Metrics
    mutable std::atomic<uint64_t> metAdded          {0};
    mutable std::atomic<uint64_t> metRejected        {0};
    mutable std::atomic<uint64_t> metEvicted         {0};
    mutable std::atomic<uint64_t> metConfirmed       {0};
    mutable std::atomic<uint64_t> metDsRejected      {0};
    mutable std::atomic<uint64_t> metOrphansAdded    {0};
    mutable std::atomic<uint64_t> metOrphansEvicted  {0};
};
