#pragma once

#include "storage/storage.h"
#include "crypto/keccak256.h"

#include <evmc/evmc.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * MedorEVMHost
 *
 * Production EVM host implementation conforming to the EVMC host interface.
 *
 * Design guarantees:
 *  - Block hash resolution is wired through a user-supplied callback.
 *  - Gas accounting is tracked per-operation using EIP-2929 costs.
 *  - Event logs are persisted in-memory and forwarded to an optional
 *    durable sink for receipt storage.
 *  - EIP-2929 access tracking is transaction-scoped and cleared on reset.
 *  - selfdestruct is guarded against reentrancy.
 *  - All hex encoding is cached per transaction frame to eliminate
 *    repeated allocations in hot paths.
 *  - A single mutex guards all mutable per-transaction state so the host
 *    is safe for concurrent access from multiple threads.
 *  - Storage failures set a persistent error flag distinguishable from
 *    logical UNCHANGED results.
 *  - All public methods are noexcept per EVMC requirements.
 */
class MedorEVMHost : public evmc::Host {
public:

    // ── Log entry ─────────────────────────────────────────────────────────────
    struct LogEntry {
        std::string                          contractHex;
        std::vector<std::array<uint8_t, 32>> topics;
        std::vector<uint8_t>                 data;
    };

    // ── Gas accounting ────────────────────────────────────────────────────────
    struct GasUsage {
        uint64_t storageReads  = 0;
        uint64_t storageWrites = 0;
        uint64_t codeReads     = 0;
        uint64_t balanceReads  = 0;
        uint64_t selfdestructs = 0;
        uint64_t total         = 0;
    };

    // ── Caller-supplied callbacks ─────────────────────────────────────────────
    using BlockHashFn = std::function<evmc_bytes32(int64_t blockNumber)>;
    using LogSinkFn   = std::function<void(const LogEntry &entry)>;

    explicit MedorEVMHost(EVMStorage &storage);

    void setBlockHashResolver(BlockHashFn fn) noexcept { blockHashFn = std::move(fn); }
    void setLogSink          (LogSinkFn   fn) noexcept { logSinkFn   = std::move(fn); }
    void setTxContext        (const evmc_tx_context &ctx) noexcept;

    // ── Transaction frame management ─────────────────────────────────────────
    void beginTransaction()  noexcept;
    void commitTransaction() noexcept;

    // ── Post-execution inspection ─────────────────────────────────────────────
    std::vector<LogEntry> getLogs()           const noexcept;
    GasUsage              getGasUsage()       const noexcept;
    bool                  hadStorageError()   const noexcept;

    // ── EVMC Host interface ───────────────────────────────────────────────────
    bool               account_exists (evmc::address addr)                       noexcept override;
    evmc_bytes32       get_storage    (evmc::address addr, evmc_bytes32 key)      noexcept override;
    evmc_storage_status set_storage   (evmc::address addr, evmc_bytes32 key,
                                       evmc_bytes32 val)                          noexcept override;
    evmc::uint256be    get_balance    (evmc::address addr)                        noexcept override;
    size_t             get_code_size  (evmc::address addr)                        noexcept override;
    evmc_bytes32       get_code_hash  (evmc::address addr)                        noexcept override;
    size_t             copy_code      (evmc::address addr, size_t code_offset,
                                       uint8_t *buffer_data,
                                       size_t   buffer_size)                      noexcept override;
    bool               selfdestruct   (evmc::address addr,
                                       evmc::address beneficiary)                 noexcept override;
    evmc_tx_context    get_tx_context ()                                          noexcept override;
    evmc_bytes32       get_block_hash (int64_t block_number)                      noexcept override;
    void               emit_log       (evmc::address addr,
                                       const uint8_t      *data,
                                       size_t              data_size,
                                       const evmc::bytes32 topics[],
                                       size_t              topics_count)          noexcept override;
    evmc_access_status access_account (evmc::address addr)                        noexcept override;
    evmc_access_status access_storage (evmc::address addr, evmc_bytes32 key)      noexcept override;

private:
    EVMStorage      &storageDB;
    evmc_tx_context  txContext{};
    BlockHashFn      blockHashFn;
    LogSinkFn        logSinkFn;

    // Single mutex guards all mutable per-transaction state so every method
    // that reads or writes the fields below is thread-safe without the caller
    // needing to coordinate external locking.
    mutable std::mutex stateMutex;

    std::vector<LogEntry>                                        logs;
    GasUsage                                                     gas{};
    bool                                                         storageErrorOccurred = false;
    bool                                                         inSelfdestruct       = false;

    // EIP-2929 access tracking — transaction-scoped
    std::unordered_set<evmc::address>                            accessedAccounts;
    std::unordered_map<evmc::address,
                       std::unordered_set<evmc_bytes32>>         accessedStorageSlots;

    // Per-transaction hex caches — cleared on beginTransaction()
    mutable std::unordered_map<evmc::address, std::string>       addrHexCache;
    mutable std::unordered_map<evmc_bytes32,  std::string>       keyHexCache;

    // EIP-2929 gas constants
    static constexpr uint64_t GAS_SLOAD_COLD    = 2100;
    static constexpr uint64_t GAS_SLOAD_WARM    = 100;
    static constexpr uint64_t GAS_SSTORE_SET    = 20000;
    static constexpr uint64_t GAS_SSTORE_RESET  = 2900;
    static constexpr uint64_t GAS_SSTORE_WARM   = 100;
    static constexpr uint64_t GAS_BALANCE_COLD  = 2600;
    static constexpr uint64_t GAS_BALANCE_WARM  = 100;
    static constexpr uint64_t GAS_EXTCODE_COLD  = 2600;
    static constexpr uint64_t GAS_EXTCODE_WARM  = 100;
    static constexpr uint64_t GAS_SELFDESTRUCT  = 5000;

    // Internal helpers — all assume stateMutex is already held by caller
    const std::string &cachedAddrHex(const evmc::address &addr) const noexcept;
    const std::string &cachedKeyHex (const evmc_bytes32  &key)  const noexcept;

    static std::string      hexEncode       (const uint8_t *data, size_t len) noexcept;
    static evmc::uint256be  uint64ToUint256be(uint64_t value)                 noexcept;
    static bool             isAllZero       (const evmc_bytes32 &v)           noexcept;

    // Decodes exactly 64 validated hex chars into 32 bytes.
    // Returns false if the string is not exactly 64 valid hex characters.
    static bool hexToBytes32(const std::string &hex, evmc_bytes32 &out) noexcept;
};
