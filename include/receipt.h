#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <shared_mutex>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/utilities/backup_engine.h>
#include <nlohmann/json.hpp>
#include <queue>

// =============================================================================
// RECEIPT LOG
// =============================================================================
struct ReceiptLog {
    std::string              address;
    std::vector<std::string> topics;
    std::vector<uint8_t>     data;
};

// =============================================================================
// TRANSACTION RECEIPT
// =============================================================================
struct TransactionReceipt {
    std::string              transactionHash;
    std::string              blockHash;
    uint64_t                 blockNumber       = 0;
    uint64_t                 transactionIndex  = 0;
    std::string              from;
    std::string              to;
    uint64_t                 gasUsed           = 0;
    uint64_t                 cumulativeGasUsed = 0;
    std::vector<ReceiptLog>  logs;
    std::array<uint8_t, 256> logsBloom{};
    bool                     status            = false;
};

// =============================================================================
// BLOOM FILTER HELPER
// =============================================================================
inline void updateBloom(std::array<uint8_t, 256>& bloom,
                         const std::string& value) noexcept
{
    for (int i = 0; i < 3; ++i) {
        size_t pos = 0;
        for (size_t j = 0; j < value.size(); ++j)
            pos = pos * 31 + static_cast<uint8_t>(value[j]) + i;
        pos %= (256 * 8);
        bloom[pos / 8] |= static_cast<uint8_t>(1 << (pos % 8));
    }
}

// =============================================================================
// RECEIPTS STORE
// =============================================================================
class ReceiptsStore {
public:

    enum class ErrorCode {
        Ok, NotFound, InvalidInput, SerialisationError,
        DbError, Timeout, SchemaMismatch, PayloadTooLarge,
        NotOpen, BackupError, VerifyError
    };

    struct Result {
        bool        ok    = false;
        ErrorCode   code  = ErrorCode::DbError;
        std::string error;

        explicit operator bool() const noexcept { return ok; }

        static Result success() noexcept
            { return {true, ErrorCode::Ok, {}}; }
        static Result failure(ErrorCode c, const std::string &msg) noexcept
            { return {false, c, msg}; }

        bool isNotFound()  const noexcept { return code == ErrorCode::NotFound; }
        bool isTimeout()   const noexcept { return code == ErrorCode::Timeout; }
        bool isPermanent() const noexcept {
            return code == ErrorCode::SchemaMismatch
                || code == ErrorCode::InvalidInput
                || code == ErrorCode::PayloadTooLarge;
        }
    };

    struct BatchItem {
        std::string    key;
        nlohmann::json value;
    };

    using LogFn = std::function<void(int level, const std::string &msg)>;

    struct Config {
        std::string  dbPath;
        std::string  backupPath;
        uint32_t     schemaVersion        = 1;
        bool         syncSingleWrites     = true;
        bool         syncBatchWrites      = false;
        size_t       maxJsonBytes         = 4 * 1024 * 1024;
        size_t       maxBatchItems        = 10000;
        uint32_t     opTimeoutMs          = 10000;
        uint32_t     autoCompactAfter     = 10000;
        uint32_t     backupAfterNWrites   = 0;
        bool         verifyOnOpen         = false;
        bool         recoverFromBackup    = false;
        size_t       timeoutWorkers       = 4;
        size_t       verifyChunkSize      = 10000;
    };

    explicit ReceiptsStore(Config cfg, LogFn logger = nullptr);
    ~ReceiptsStore();

    ReceiptsStore(const ReceiptsStore &)            = delete;
    ReceiptsStore &operator=(const ReceiptsStore &) = delete;

    bool isOpen() const noexcept;

    Result put   (const std::string &key,
                  const nlohmann::json &value,
                  bool sync)                                     noexcept;
    Result put   (const std::string &key,
                  const nlohmann::json &value)                   noexcept;
    Result get   (const std::string &key,
                  nlohmann::json &value)                   const noexcept;
    Result remove(const std::string &key, bool sync)             noexcept;
    Result remove(const std::string &key)                        noexcept;

    Result putBatch(const std::vector<BatchItem> &items,
                    bool sync)                                   noexcept;
    Result putBatch(const std::vector<BatchItem> &items)         noexcept;

    void   compact()                                             noexcept;
    Result verify (size_t chunkSize = 0)                        noexcept;
    Result backup ()                                             noexcept;
    Result recover()                                             noexcept;

private:

    struct DbDeleter {
        void operator()(rocksdb::DB *p) const noexcept {
            if (!p) return;
            for (int i = 0; i < 2; ++i) {
                if (p->FlushWAL(true).ok()) break;
            }
            delete p;
        }
    };
    using DbPtr = std::unique_ptr<rocksdb::DB, DbDeleter>;

    Config            cfg_;
    LogFn             logger_;
    DbPtr             db_;
    rocksdb::Options  options_;

    mutable std::shared_mutex rwMutex_;

    mutable std::mutex                                backupMutex_;
    std::unique_ptr<rocksdb::BackupEngine>            backupEngine_;
    std::unique_ptr<rocksdb::BackupEngineReadOnly>    restoreEngine_;
    bool                                              backupEngineOpen_  = false;
    bool                                              restoreEngineOpen_ = false;

    std::atomic<bool>     compacting_  { false };
    std::atomic<uint32_t> deleteCount_ { 0 };
    std::atomic<uint32_t> writeCount_  { 0 };

    struct Worker {
        std::thread                        thread;
        std::queue<std::function<void()>>  queue;
        std::mutex                         mtx;
        std::condition_variable            cv;
        bool                               stop = false;
    };
    std::vector<std::unique_ptr<Worker>> pool_;
    std::atomic<size_t>                  poolIdx_{ 0 };

    static constexpr const char *SCHEMA_KEY =
        "__receipts_schema_version__";

    void   log(int level, const std::string &msg)   const noexcept;
    Result executeWithTimeout(std::function<Result()> fn) const noexcept;
    bool   initSchemaVersion()                             noexcept;
    bool   openDb(bool attemptRecovery)                    noexcept;
    void   maybeBackup()                                   noexcept;
    void   maybeCompact()                                  noexcept;
    bool   ensureBackupEngine()                            noexcept;
    bool   ensureRestoreEngine()                           noexcept;

    static Result fromStatus(const rocksdb::Status &s,
                              const std::string &ctx)      noexcept;
    static bool   isValidKey(const std::string &key)       noexcept;
};
