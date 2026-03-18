#pragma once

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <functional>
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

    static constexpr size_t      MAX_KEY_LEN = 256;
    static constexpr const char* HEALTH_KEY  = "__health__";

    explicit AccountDB(const std::string& path);
    ~AccountDB();
    AccountDB(const AccountDB&)            = delete;
    AccountDB& operator=(const AccountDB&) = delete;

    bool isOpen()    const noexcept;
    bool isHealthy() const noexcept;

    Result put(const std::string& key,
               const std::string& value,
               bool sync = true)  noexcept;

    Result get(const std::string& key,
               std::string& valueOut)    noexcept;

    Result del(const std::string& key,
               bool sync = true)  noexcept;

    Result writeBatch(
        const std::vector<std::pair<std::string, std::string>>& items,
        bool sync = true) noexcept;

    Result deleteBatch(
        const std::vector<std::string>& keys,
        bool sync = true) noexcept;

    int64_t iteratePrefix(const std::string&  prefix,
                          const ScanCallback& callback,
                          size_t              maxResults = 0) noexcept;

    void setLogger(LoggerFn fn) { logger = std::move(fn); }

private:
    rocksdb::DB*              db = nullptr;
    rocksdb::Options          options;
    mutable std::shared_mutex dbMutex;
    LoggerFn                  logger;

    void   log(int level, const std::string& msg)           const noexcept;
    Result fromStatus(const rocksdb::Status& s,
                      const std::string& ctx)                     noexcept;
    static bool isValidKey(const std::string& key)                noexcept;
};
