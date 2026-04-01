#pragma once

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <functional>
#include <limits>
#include <shared_mutex>
#include <string>
#include <vector>

class AccountDB {
public:
    struct Result {
        bool        ok    = true;
        std::string error;
        static Result success()                     { return {true,  ""}; }
        static Result failure(const std::string& e) { return {false, e}; }
        explicit operator bool() const { return ok; }
    };

    using ScanCallback = std::function<bool(const std::string&,
                                             const std::string&)>;
    using LoggerFn     = std::function<void(int, const std::string&)>;

    static constexpr size_t      MAX_KEY_LEN = 4096;
    static constexpr const char* HEALTH_KEY  = "__health__";

    explicit AccountDB(const std::string& path);
    ~AccountDB();
    AccountDB(const AccountDB&)            = delete;
    AccountDB& operator=(const AccountDB&) = delete;

    bool isOpen()    const noexcept;
    bool isHealthy() const noexcept;

    Result put(const std::string& key,
               const std::string& value,
               bool sync = true) noexcept;

    Result get(const std::string& key,
               std::string& valueOut) const noexcept;

    Result del(const std::string& key,
               bool sync = true) noexcept;

    Result writeBatch(
        const std::vector<std::pair<std::string,
                                    std::string>>& items,
        bool sync = true) noexcept;

    Result deleteBatch(
        const std::vector<std::string>& keys,
        bool sync = true) noexcept;

    int64_t iteratePrefix(const std::string&  prefix,
                          const ScanCallback& callback,
                          size_t              maxResults = 0) noexcept;

    uint64_t getBalance   (const std::string& address)                  const noexcept;
    bool     setBalance   (const std::string& address, uint64_t amount)       noexcept;
    bool     addBalance   (const std::string& address, uint64_t amount)       noexcept;
    bool     deductBalance(const std::string& address, uint64_t amount)       noexcept;

    uint64_t getNonce      (const std::string& address)                  const noexcept;
    bool     incrementNonce(const std::string& address)                        noexcept;
    bool     validateNonce (const std::string& address,
                             uint64_t           txNonce)                  const noexcept;

    bool                 setContractCode(const std::string&           address,
                                          const std::vector<uint8_t>& code)   noexcept;
    std::vector<uint8_t> getContractCode(const std::string&           address) const noexcept;
    bool                 setStorageSlot (const std::string&           address,
                                          const std::string&           slot,
                                          const std::string&           value)  noexcept;
    std::string          getStorageSlot (const std::string&           address,
                                          const std::string&           slot)    const noexcept;

    bool        setStateRoot(uint64_t blockHeight,
                              const std::string& stateRoot)       noexcept;
    std::string getStateRoot(uint64_t blockHeight)          const noexcept;
    bool        setMeta     (uint64_t blockHeight,
                              const std::string& key,
                              const std::string& value)           noexcept;
    std::string getMeta     (uint64_t blockHeight,
                              const std::string& key)       const noexcept;

    Result commitBlockState(
        uint64_t blockHeight,
        const std::vector<std::pair<std::string,
                                    uint64_t>>& balanceChanges,
        const std::vector<std::string>&          nonceIncrements,
        const std::string&                       stateRoot) noexcept;

    bool     addGasUsed     (const std::string& address,
                              uint64_t           gasUsed)  noexcept;
    uint64_t getTotalGasUsed(const std::string& address)  const noexcept;

    bool accountExists(const std::string& address) const noexcept;

    void setLogger(LoggerFn fn) { logger = std::move(fn); }

private:
    rocksdb::DB*              db = nullptr;
    rocksdb::Options          options;
    mutable std::shared_mutex dbMutex;
    LoggerFn                  logger;

    void   log(int level, const std::string& msg)     const noexcept;
    Result fromStatus(const rocksdb::Status& s,
                      const std::string&     ctx)           noexcept;
    static bool isValidKey(const std::string& key)          noexcept;
};
