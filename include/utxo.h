#pragma once

#include "transaction.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// =============================================================================
// UTXO STRUCT
// =============================================================================
struct UTXO {
    std::string txHash;
    int         outputIndex = 0;
    uint64_t    value       = 0;
    std::string address;
    uint64_t    blockHeight = 0;
    bool        isCoinbase  = false;
};

// =============================================================================
// UTXO SET
//
// Thread safety:
//   - rwMutex_ (shared_mutex) protects utxos_ and addressIndex_.
//   - Read operations use shared_lock — high concurrency for queries.
//   - Write operations use unique_lock — prevents races on mutations.
//   - persist() called while write lock is held — consistent snapshot.
//   - Async persist mode: background thread drains snapshot queue,
//     calls persistFn_ outside the write lock — no I/O blocking.
//   - Secondary index (addressIndex_) maintained in sync with utxos_
//     under the same write lock — never out of step.
//   - Metrics counters are atomics — no lock needed for reads.
//
// Synchronization with group-one components:
//   - Mempool: validates inputs via getUTXO() and isUnspent()
//     under shared_lock — never blocks addTransaction.
//   - Blockchain: calls addUTXO/spendUTXO under its own block lock;
//     UTXOSet uses its own rwMutex_ — two independent lock domains.
//   - Network/PeerManager: no direct UTXO access — validated indirectly
//     through Mempool and Blockchain before broadcast.
//
// Large-scale features:
//   - Reserve(1M) at construction avoids rehash under load.
//   - Async persist mode decouples disk I/O from write-lock duration.
//   - Prometheus metrics endpoint for observability.
//   - loadSnapshot() for atomic UTXO state restore and rollback.
//   - Coinbase maturity enforcement with UINT64_MAX rollback sentinel.
//   - Overflow-safe getBalance() returns nullopt on uint64_t overflow.
// =============================================================================
class UTXOSet {
public:
    static constexpr uint64_t COINBASE_MATURITY = 100;

    using PersistFn = std::function<void(
        const std::unordered_map<std::string, UTXO>&)>;
    using LogFn     = std::function<void(int, const std::string&)>;

    // =========================================================================
    // CONFIG
    // =========================================================================
    struct Config {
        bool   asyncPersist         = false;
        size_t maxPersistQueueDepth = 8;
    };

    // =========================================================================
    // METRICS
    // =========================================================================
    struct Metrics {
        uint64_t utxosAdded               = 0;
        uint64_t utxosSpent               = 0;
        uint64_t totalValueTracked        = 0;
        uint64_t coinbaseMaturityRejected = 0;
        size_t   currentUtxoCount         = 0;
        size_t   currentAddressCount      = 0;
    };

    // =========================================================================
    // LIFECYCLE
    // =========================================================================
    explicit UTXOSet(PersistFn fn = nullptr) noexcept;
    ~UTXOSet() { stopPersistWorker(); }

    UTXOSet(const UTXOSet&)            = delete;
    UTXOSet& operator=(const UTXOSet&) = delete;

    void configure(Config cfg) noexcept {
        cfg_ = std::move(cfg);
        if (cfg_.asyncPersist) startPersistWorker();
    }

    void setLogger(LogFn fn) noexcept;

    // =========================================================================
    // CORE OPERATIONS
    // =========================================================================

    // Add a new unspent output.
    // Returns false on duplicate key, empty txHash, negative index,
    // or empty address.
    bool addUTXO(const TxOutput&    output,
                  const std::string& txHash,
                  int                outputIndex,
                  uint64_t           blockHeight,
                  bool               isCoinbase = false) noexcept;

    // Mark an output as spent.
    // Pass currentBlockHeight = UINT64_MAX to bypass coinbase maturity
    // (used by rollback paths in Blockchain).
    bool spendUTXO(const std::string& txHash,
                    int                outputIndex,
                    uint64_t           currentBlockHeight) noexcept;

    // =========================================================================
    // QUERIES — all thread-safe under shared_lock
    // =========================================================================
    std::optional<uint64_t> getBalance(
        const std::string& address)                        const noexcept;
    std::vector<UTXO>       getUTXOsForAddress(
        const std::string& address)                        const noexcept;
    std::optional<UTXO>     getUTXO(
        const std::string& txHash, int outputIndex)        const noexcept;
    bool                    isUnspent(
        const std::string& txHash, int outputIndex)        const noexcept;
    size_t                  size()                         const noexcept;

    // =========================================================================
    // STATE MANAGEMENT
    // =========================================================================
    void clear()                                                  noexcept;
    bool loadSnapshot(
        const std::unordered_map<std::string, UTXO>& snapshot)   noexcept;

    // =========================================================================
    // METRICS AND OBSERVABILITY
    // =========================================================================
    Metrics     getMetrics()       const noexcept;
    std::string getPrometheusText() const noexcept;

    // =========================================================================
    // STATIC HELPERS
    // =========================================================================
    static std::string makeKey(const std::string& txHash,
                                int outputIndex)           noexcept;

private:
    // =========================================================================
    // PRIMARY STORAGE
    // Both maps protected by rwMutex_.
    // =========================================================================
    mutable std::shared_mutex                              rwMutex_;
    std::unordered_map<std::string, UTXO>                  utxos_;
    std::unordered_map<std::string, std::vector<std::string>> addressIndex_;

    PersistFn persistFn_;
    Config    cfg_;

    // =========================================================================
    // ASYNC PERSIST WORKER
    // When cfg_.asyncPersist=true, persist() enqueues a snapshot copy.
    // Background thread calls persistFn_ outside the write lock.
    // Only the latest snapshot in the queue is flushed (older ones dropped).
    // =========================================================================
    mutable std::mutex                                     persistQueueMu_;
    mutable std::condition_variable                        persistQueueCv_;
    mutable std::vector<std::unordered_map<std::string, UTXO>> persistQueue_;
    std::atomic<bool>                                      persistWorkerStopped_{true};
    std::thread                                            persistWorkerThread_;

    void startPersistWorker() noexcept;
    void stopPersistWorker()  noexcept;
    void runPersistWorker()   noexcept;

    // =========================================================================
    // INDEX HELPERS — caller must hold write lock
    // =========================================================================
    void indexAdd   (const std::string& address,
                      const std::string& key) noexcept;
    void indexRemove(const std::string& address,
                      const std::string& key) noexcept;

    void persist() const noexcept;

    // =========================================================================
    // LOGGER
    // =========================================================================
    mutable std::mutex logMu_;
    LogFn              logFn_;
    void slog(int level, const std::string& msg) const noexcept;

    // =========================================================================
    // METRICS
    // =========================================================================
    struct MetricsState {
        std::atomic<uint64_t> utxosAdded              {0};
        std::atomic<uint64_t> utxosSpent              {0};
        std::atomic<uint64_t> totalValueTracked        {0};
        std::atomic<uint64_t> coinbaseMaturityRejected {0};
    };
    mutable MetricsState metrics_;
};
