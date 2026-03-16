#pragma once

#include "rocksdb_wrapper.h"

#include <array>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

/**
 * EVMStorage
 *
 * Persistent storage layer for EVM contract bytecode, storage slots,
 * and account balances backed by RocksDB.
 *
 * Design guarantees:
 *  - Addresses enforced as exactly 40 valid hex characters (20 bytes).
 *  - Storage slots and values enforced as exactly 64 valid hex characters
 *    (32 bytes), with full character-level hex validation on every write.
 *  - Balances stored as 32-byte big-endian (full 256-bit EVM precision).
 *  - Per-slot fine-grained locking via a slot-keyed mutex map, preventing
 *    independent slots from blocking each other unnecessarily.
 *  - Contract code is stored with a CRC32 checksum and an explicit-endian
 *    length prefix for corruption detection beyond size checks alone.
 *  - All RocksDB calls are wrapped in try/catch so a throwing wrapper
 *    implementation never propagates to the caller.
 *  - Schema version is written on first open and verified on subsequent
 *    opens to detect incompatible layout changes.
 *  - No method throws. All failures are communicated via ReadStatus or bool.
 */
class EVMStorage {
public:

    // Full 256-bit unsigned integer, big-endian, matching EVM word size
    using uint256 = std::array<uint8_t, 32>;

    // Distinguishes all failure modes so callers never silently receive
    // a zero or empty value when the real cause is corruption or infrastructure
    enum class ReadStatus {
        OK,
        NOT_FOUND,
        CORRUPT,
        DB_ERROR
    };

    // One item in an atomic batch storage write
    struct StorageBatchItem {
        std::string addrHex;    // exactly 40 valid hex chars
        std::string slotHex;    // exactly 64 valid hex chars
        std::string valueHex;   // exactly 64 valid hex chars
    };

    explicit EVMStorage(const std::string &dbPath);

    EVMStorage(const EVMStorage &)            = delete;
    EVMStorage &operator=(const EVMStorage &) = delete;

    // ── Contract Bytecode ─────────────────────────────────────────────────────
    bool       putContractCode(const std::string &addrHex,
                               const std::vector<uint8_t> &code);
    ReadStatus getContractCode(const std::string &addrHex,
                               std::vector<uint8_t> &codeOut);

    // ── Contract Storage Slots ────────────────────────────────────────────────
    bool       putContractStorage(const std::string &addrHex,
                                  const std::string &slotHex,
                                  const std::string &valueHex);
    ReadStatus getContractStorage(const std::string &addrHex,
                                  const std::string &slotHex,
                                  std::string &valueOut);
    bool       deleteContractStorage(const std::string &addrHex,
                                     const std::string &slotHex);

    // All items committed atomically — all succeed or none do
    bool putContractStorageBatch(const std::vector<StorageBatchItem> &items);

    // ── Balances (full 256-bit) ───────────────────────────────────────────────
    bool       putBalance  (const std::string &addrHex, const uint256 &balance);
    ReadStatus getBalance  (const std::string &addrHex, uint256 &balanceOut);

    // 64-bit wrappers for callers that do not need full 256-bit precision
    bool       putBalance64(const std::string &addrHex, uint64_t balance);
    ReadStatus getBalance64(const std::string &addrHex, uint64_t &balanceOut);

private:
    RocksDBWrapper rocksdb;

    // Per-slot fine-grained locking
    // Each unique DB key gets its own shared_mutex, eliminating false contention
    // between independent slots that happen to share an address stripe.
    mutable std::mutex               slotMapMutex;
    mutable std::unordered_map<std::string,
                               std::shared_mutex> slotMutexMap;

    std::shared_mutex &mutexForKey(const std::string &dbKey) const;

    // Schema versioning
    static constexpr uint32_t SCHEMA_VERSION = 1;
    bool initSchemaVersion();

    // Validation
    static bool isValidHex    (const std::string &s, size_t requiredLen) noexcept;
    static bool isValidAddrHex(const std::string &s) noexcept;
    static bool isValidSlotHex(const std::string &s) noexcept;

    // Checksum
    static uint32_t crc32c(const uint8_t *data, size_t length) noexcept;

    // Key builders
    static std::string codeKey   (const std::string &addr)
        { return "code:"    + addr; }
    static std::string storageKey(const std::string &addr,
                                  const std::string &slot)
        { return "storage:" + addr + ":" + slot; }
    static std::string balanceKey(const std::string &addr)
        { return "balance:" + addr; }
    static std::string schemaKey ()
        { return "__schema_version__"; }
};
