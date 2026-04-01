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
// =============================================================================
class Mempool {
public:

    struct Config {
        size_t      maxTxCount              = 100000;
        uint64_t    maxTotalBytes           = 256ULL * 1024 * 1024;
        uint64_t    minFeePerGas            = 1;
        uint64_t    maxTxAgeSeconds         = 3600;
        bool        persistToDisk           = false;
        std::string dbPath;
        size_t      orphanMaxCount          = 1000;
        uint64_t    orphanMaxAgeSecs        = 300;
        size_t      persistBatchSize        = 512;
        size_t      maxPersistQueueSize     = 50000;
        int64_t     persistWriteBytesPerSec = 50 * 1024 * 1024;
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

    void setLogger (LogFn   fn);
    void onEviction(EvictFn fn);

    // =========================================================================
    // CORE OPERATIONS
    // =========================================================================
    bool addTransaction   (const Transaction&              tx,
                            uint64_t                        baseFee);
    void removeTransaction(const std::string&              txHash);
    void removeConfirmed  (const std::vector<Transaction>& block);
    void reorgRestore     (const std::vector<Transaction>& reorgedOut);

    // =========================================================================
    // QUERIES
    // =========================================================================
    bool                       hasTransaction (const std::string& txHash) const noexcept;
    std::optional<Transaction> getTransaction (const std::string& txHash) const noexcept;
    std::vector<Transaction>   getTransactions()                           const noexcept;
    std::vector<Transaction>   getPendingTxs  ()                           const noexcept;
    std::vector<Transaction>   getSortedByFee (size_t   maxCount,
                                                uint64_t baseFee)          const noexcept;
    size_t                     txCount()       const noexcept;
    size_t                     totalBytes()    const noexcept;
    uint64_t                   minFee()        const noexcept;

    // =========================================================================
    // MAINTENANCE
    // =========================================================================
    void evictStale    (uint64_t baseFee)    noexcept;
    void evictLowFee   (uint64_t newBaseFee) noexcept;
    void trimToCapacity(uint64_t baseFee)    noexcept;

    // =========================================================================
    // METRICS
    // =========================================================================
    Metrics getMetrics() const noexcept;

    // =========================================================================
    // STATIC HELPERS
    // =========================================================================
    static size_t   estimateTxBytes(const Transaction& tx)              noexcept;
    static uint64_t effectiveFee   (const Transaction& tx,
                                     uint64_t           baseFee)         noexcept;

private:

    Config         cfg_;
    const UTXOSet& utxoSet_;

    mutable std::shared_mutex                        mu_;
    std::unordered_map<std::string, MempoolEntry>    entries_;
    std::unordered_map<std::string, MempoolEntry>    orphans_;
    std::unordered_set<std::string>                  spentOutpoints_;
    std::atomic<uint64_t>                            totalBytes_{0};

    static constexpr size_t STRIPE_COUNT = 128;
    mutable std::array<std::mutex, STRIPE_COUNT> stripes_;
    std::mutex& stripeFor(const std::string& txHash) noexcept;

    std::unique_ptr<rocksdb::DB> db_;

    enum class PersistOp { Put, Delete };
    struct PersistJob {
        PersistOp   op;
        std::string key;
        std::string value;
    };
    mutable std::mutex      persistQueueMu_;
    std::condition_variable persistQueueCv_;
    std::deque<PersistJob>  persistQueue_;
    std::atomic<bool>       persistStopped_;
    std::thread             persistThread_;

    void enqueuePersist  (const Transaction& tx)     noexcept;
    void enqueueUnpersist(const std::string& txHash) noexcept;
    void runPersistWorker()                           noexcept;
    void loadFromDisk()                               noexcept;

    static std::string serializeTx(const Transaction& tx) noexcept;

    mutable std::mutex logMu_;
    mutable std::mutex evictMu_;
    LogFn              logFn_;
    EvictFn            evictFn_;

    void slog(int level, const std::string& msg) const noexcept;

    bool validateTx        (const Transaction& tx,
                             uint64_t           baseFee) const noexcept;
    bool isDoubleSpend     (const Transaction& tx)       const noexcept;
    bool checkNonceOrdering(const Transaction& tx)       const noexcept;

    void addOrphanLocked  (const Transaction& tx)                noexcept;
    void promoteOrphans   (const std::string& confirmedTxHash,
                            uint64_t           baseFee)           noexcept;
    void evictStaleOrphans()                                      noexcept;

    mutable std::atomic<uint64_t> metAdded         {0};
    mutable std::atomic<uint64_t> metRejected      {0};
    mutable std::atomic<uint64_t> metEvicted       {0};
    mutable std::atomic<uint64_t> metConfirmed     {0};
    mutable std::atomic<uint64_t> metDsRejected    {0};
    mutable std::atomic<uint64_t> metOrphansAdded  {0};
    mutable std::atomic<uint64_t> metOrphansEvicted{0};
};
