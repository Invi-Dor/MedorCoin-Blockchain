#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <array>

// Forward declaration for database batching to keep header clean
namespace rocksdb { class WriteBatch; }

// =============================================================================
// CONSTANTS & LOG LEVELS
// =============================================================================
constexpr size_t NUM_SHARDS = 64; 
enum LogLevel { LOG_INFO = 1, LOG_WARNING = 2, LOG_CRITICAL = 3 };

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
    bool        isLocked    = false;
    std::string lockedForAddress = ""; 
};

// =============================================================================
// UTXO SET CLASS (SHARDED & ATOMIC)
// =============================================================================
class UTXOSet {
public:
    struct Metrics {
        uint64_t utxosAdded;
        uint64_t utxosSpent;
        uint64_t totalValueTracked;
        uint64_t coinbaseMaturityRejected;
    };

    using LogFn = std::function<void(int level, const std::string& msg)>;

    explicit UTXOSet() noexcept;
    ~UTXOSet() = default;

    // Disable copy/move for safety in threaded environments
    UTXOSet(const UTXOSet&) = delete;
    UTXOSet& operator=(const UTXOSet&) = delete;

    void setLogger(LogFn fn) noexcept { logFn_ = fn; }

    // Core Operations (Now supporting Atomic DB Batches)
    [[nodiscard]] bool addUTXO(const std::string& address, const std::string& txHash, int outputIndex, 
                               uint64_t amount, uint64_t blockHeight, bool isCoinbase, 
                               rocksdb::WriteBatch& dbBatch) noexcept;

    [[nodiscard]] bool spendUTXO(const std::string& txHash, int outputIndex, uint64_t amount, 
                                 uint64_t currentBlockHeight, rocksdb::WriteBatch& dbBatch) noexcept;

    // Queries (Thread-safe via Shard Locks)
    [[nodiscard]] std::optional<uint64_t> getBalance(const std::string& address) const noexcept;
    [[nodiscard]] std::vector<UTXO>       getUTXOsForAddress(const std::string& address) const noexcept;
    [[nodiscard]] std::optional<UTXO>     getUTXO(const std::string& txHash, int outputIndex) const noexcept;

    // Metrics
    [[nodiscard]] Metrics getMetrics() const noexcept;

private:
    static constexpr uint64_t COINBASE_MATURITY = 100;

    // 1. Sharded Data Structures (Eliminates Global Bottlenecks)
    // Main UTXO Storage: Sharded by UTXO Key (txHash + index)
    std::array<std::shared_mutex, NUM_SHARDS> shardMutexes_;
    std::array<std::unordered_map<std::string, UTXO>, NUM_SHARDS> utxoShards_;

    // Address Index: Sharded by Wallet Address
    std::array<std::shared_mutex, NUM_SHARDS> addressIndexMutexes_;
    std::array<std::unordered_map<std::string, std::unordered_set<std::string>>, NUM_SHARDS> addressToUtxoIndex_;

    // 2. Atomic Metrics
    struct {
        std::atomic<uint64_t> utxosAdded{0};
        std::atomic<uint64_t> utxosSpent{0};
        std::atomic<uint64_t> totalValueTracked{0};
        std::atomic<uint64_t> coinbaseMaturityRejected{0};
    } metrics_;

    LogFn logFn_;

    // 3. Private Helpers
    static std::string makeKey(const std::string& txHash, int outputIndex) noexcept;
    void addAddressIndexSafe(const std::string& address, const std::string& utxoKey) noexcept;
    void removeAddressIndexSafe(const std::string& address, const std::string& utxoKey) noexcept;
    void slog(int level, const std::string& msg) const noexcept;
};
