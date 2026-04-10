#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <condition_variable>
#include "transaction.h"


// =============================================================================
// UTXO STRUCT
// =============================================================================

struct UTXO {
    std::string txHash;
    int         outputIndex = -1;
    uint64_t    value       = 0;
    std::string address;
    uint64_t    blockHeight = 0;
    bool        isCoinbase  = false;
    
    // Cross-chain swap properties
    bool        isLocked    = false;
    std::string lockedForAddress = ""; // EVM address receiving wrapped tokens
};

// =============================================================================
// UTXO SET CLASS
// =============================================================================
class UTXOSet {
public:
    struct Metrics {
        uint64_t utxosAdded;
        uint64_t utxosSpent;
        size_t   currentUtxoCount;
        size_t   currentAddressCount;
        uint64_t totalValueTracked;
        uint64_t coinbaseMaturityRejected;
    };

    struct Config {
        bool   asyncPersist         = true;
        size_t maxPersistQueueDepth = 100;
    };

    using PersistFn = std::function<void(const std::unordered_map<std::string, UTXO>&)>;
    using LogFn     = std::function<void(int level, const std::string& msg)>;

    // Constructor
    explicit UTXOSet(PersistFn fn = nullptr) noexcept;
    ~UTXOSet() { stopPersistWorker(); }

    // Disable copy/move
    UTXOSet(const UTXOSet&) = delete;
    UTXOSet& operator=(const UTXOSet&) = delete;

    // Configuration & Logging
    void setLogger(LogFn fn) noexcept;
    void startPersistWorker() noexcept;
    void stopPersistWorker() noexcept;

    // Core UTXO Operations
    [[nodiscard]] bool addUTXO(const TxOutput& output, const std::string& txHash, int outputIndex, uint64_t blockHeight, bool isCoinbase = false) noexcept;
    [[nodiscard]] bool spendUTXO(const std::string& txHash, int outputIndex, uint64_t currentBlockHeight = std::numeric_limits<uint64_t>::max()) noexcept;
    
    // Cross-Chain Swap Operations
    [[nodiscard]] bool lockUTXO(const std::string& txHash, int outputIndex, const std::string& evmAddress) noexcept;
    [[nodiscard]] bool unlockUTXO(const std::string& txHash, int outputIndex) noexcept;

    // Queries
    [[nodiscard]] std::optional<uint64_t> getBalance(const std::string& address) const noexcept;
    [[nodiscard]] std::vector<UTXO>       getUTXOsForAddress(const std::string& address) const noexcept;
    [[nodiscard]] std::optional<UTXO>     getUTXO(const std::string& txHash, int outputIndex) const noexcept;
    [[nodiscard]] bool                    isUnspent(const std::string& txHash, int outputIndex) const noexcept;
    [[nodiscard]] size_t                  size() const noexcept;

    // State Management
    void clear() noexcept;
    [[nodiscard]] bool loadSnapshot(const std::unordered_map<std::string, UTXO>& snapshot) noexcept;

    // Metrics
    [[nodiscard]] Metrics     getMetrics() const noexcept;
    [[nodiscard]] std::string getPrometheusText() const noexcept;

private:
    static constexpr uint64_t COINBASE_MATURITY = 100;

    Config    cfg_;
    PersistFn persistFn_;
    LogFn     logFn_;
    mutable std::mutex logMu_;

    mutable std::shared_mutex rwMutex_;
    std::unordered_map<std::string, UTXO> utxos_;
    std::unordered_map<std::string, std::vector<std::string>> addressIndex_;

    // Async Persist Worker
    mutable std::mutex              persistQueueMu_;
    mutable std::condition_variable persistQueueCv_;
    mutable std::vector<std::unordered_map<std::string, UTXO>> persistQueue_;
    std::thread                     persistWorkerThread_;
    std::atomic<bool>               persistWorkerStopped_{true};

    // Atomic Metrics
    struct {
        std::atomic<uint64_t> utxosAdded{0};
        std::atomic<uint64_t> utxosSpent{0};
        std::atomic<uint64_t> totalValueTracked{0};
        std::atomic<uint64_t> coinbaseMaturityRejected{0};
    } metrics_;

    // Helpers
    static std::string makeKey(const std::string& txHash, int outputIndex) noexcept;
    void indexAdd(const std::string& address, const std::string& key) noexcept;
    void indexRemove(const std::string& address, const std::string& key) noexcept;
    void persist() const noexcept;
    void runPersistWorker() noexcept;
    void slog(int level, const std::string& msg) const noexcept;
};
