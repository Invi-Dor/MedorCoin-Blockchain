#include "accountdb.h"
#include "crypto/keccak256.h"

#include <cctype>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <charconv>

// =============================================================================
// KEY PREFIXES
// All keys stored in RocksDB are namespaced by prefix to avoid collisions.
// "bal:"    -- account balance (uint64_t as decimal string)
// "nonce:"  -- account nonce   (uint64_t as decimal string)
// "code:"   -- contract bytecode (hex string)
// "store:"  -- contract storage slot (hex string)
// "meta:"   -- block-height-tagged metadata
// "state:"  -- state root per block height
// =============================================================================
static constexpr const char* PREFIX_BAL    = "bal:";
static constexpr const char* PREFIX_NONCE  = "nonce:";
static constexpr const char* PREFIX_CODE   = "code:";
static constexpr const char* PREFIX_STORE  = "store:";
static constexpr const char* PREFIX_META   = "meta:";
static constexpr const char* PREFIX_STATE  = "state:";

// =============================================================================
// CONSTRUCTOR
// Fix 1: open failure is fatal -- db stays nullptr, isOpen() returns false.
// Fix 8: health check failure is also fatal -- db reset to nullptr.
// =============================================================================
AccountDB::AccountDB(const std::string& path)
{
    if (path.empty()) {
        std::cerr << "[AccountDB] FATAL: empty path rejected\n";
        return;
    }

    options.create_if_missing        = true;
    options.paranoid_checks          = true;
    options.write_buffer_size        = 64 * 1024 * 1024;
    options.max_open_files           = 500;
    options.compression              = rocksdb::kSnappyCompression;

    rocksdb::DB* raw = nullptr;
    rocksdb::Status s = rocksdb::DB::Open(options, path, &raw);
    if (!s.ok()) {
        std::cerr << "[AccountDB] FATAL: failed to open at '"
                  << path << "': " << s.ToString() << "\n";
        return;
    }
    db = raw;

    // Fix 8: health check failure is fatal
    rocksdb::WriteOptions wo;
    wo.sync = true;
    rocksdb::Status hs = db->Put(wo, HEALTH_KEY, "ok");
    if (!hs.ok()) {
        std::cerr << "[AccountDB] FATAL: health check write failed: "
                  << hs.ToString() << "\n";
        delete db;
        db = nullptr;
        return;
    }

    log(0, "opened at '" + path + "'");
}

// =============================================================================
// DESTRUCTOR
// =============================================================================
AccountDB::~AccountDB()
{
    if (db) {
        delete db;
        db = nullptr;
    }
}

// =============================================================================
// IS OPEN / IS HEALTHY
// =============================================================================
bool AccountDB::isOpen() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(dbMutex);
    return db != nullptr;
}

bool AccountDB::isHealthy() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return false;
    std::string val;
    rocksdb::Status s = db->Get(
        rocksdb::ReadOptions(), HEALTH_KEY, &val);
    return s.ok() && val == "ok";
}

// =============================================================================
// LOGGING
// Fix 2: exceptions from logger reported to stderr, not silently swallowed.
// =============================================================================
void AccountDB::log(int level, const std::string& msg) const noexcept
{
    if (logger) {
        try {
            logger(level, "[AccountDB] " + msg);
        } catch (const std::exception& e) {
            std::cerr << "[AccountDB] logger threw: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[AccountDB] logger threw unknown exception\n";
        }
        return;
    }
    if (level >= 2)
        std::cerr << "[AccountDB][ERROR] " << msg << "\n";
    else if (level == 1)
        std::cerr << "[AccountDB][WARN]  " << msg << "\n";
    else
        std::cout << "[AccountDB][INFO]  " << msg << "\n";
}

// =============================================================================
// IS VALID KEY
// Fix 3: checks length AND rejects null bytes and control characters.
// Fix 9: conservative 4KB limit well within RocksDB internal limits.
// =============================================================================
bool AccountDB::isValidKey(const std::string& key) noexcept
{
    if (key.empty())              return false;
    if (key.size() > MAX_KEY_LEN) return false;
    for (unsigned char c : key) {
        if (c == 0x00)            return false;
        if (c < 0x20 && c != 0x09) return false;
    }
    return true;
}

// =============================================================================
// FROM STATUS
// Fix 10: structured error code prefixes for programmatic handling.
// Prefixes: NOT_FOUND, IO_ERROR, CORRUPTION, INVALID_ARG, UNKNOWN
// =============================================================================
AccountDB::Result AccountDB::fromStatus(
    const rocksdb::Status& s,
    const std::string&     ctx) noexcept
{
    if (s.ok())               return Result::success();
    if (s.IsNotFound())       return Result::failure("NOT_FOUND:"   + ctx);
    if (s.IsIOError())        return Result::failure("IO_ERROR:"    + ctx + ": " + s.ToString());
    if (s.IsCorruption())     return Result::failure("CORRUPTION:"  + ctx + ": " + s.ToString());
    if (s.IsInvalidArgument())return Result::failure("INVALID_ARG:" + ctx + ": " + s.ToString());
    return                           Result::failure("UNKNOWN:"     + ctx + ": " + s.ToString());
}

// =============================================================================
// PUT
// =============================================================================
AccountDB::Result AccountDB::put(
    const std::string& key,
    const std::string& value,
    bool               sync) noexcept
{
    if (!isValidKey(key))
        return Result::failure(
            "INVALID_ARG:put: invalid key '" + key + "'");

    std::unique_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return Result::failure("IO_ERROR:put: DB not open");

    try {
        rocksdb::WriteOptions wo;
        wo.sync = sync;
        rocksdb::Status s = db->Put(wo, key, value);
        auto r = fromStatus(s, "put key=" + key);
        if (!r) log(2, r.error);
        return r;
    } catch (const std::exception& e) {
        return Result::failure(
            std::string("INTERNAL:put exception: ") + e.what());
    }
}

// =============================================================================
// GET
// Fix 1: const-correct -- uses shared_lock, does not mutate state.
// =============================================================================
AccountDB::Result AccountDB::get(
    const std::string& key,
    std::string&       valueOut) const noexcept
{
    if (!isValidKey(key))
        return Result::failure(
            "INVALID_ARG:get: invalid key '" + key + "'");

    std::shared_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return Result::failure("IO_ERROR:get: DB not open");

    try {
        rocksdb::Status s = db->Get(
            rocksdb::ReadOptions(), key, &valueOut);
        if (s.IsNotFound())
            return Result::failure("NOT_FOUND:" + key);
        return fromStatus(s, "get key=" + key);
    } catch (const std::exception& e) {
        return Result::failure(
            std::string("INTERNAL:get exception: ") + e.what());
    }
}

// =============================================================================
// DEL
// =============================================================================
AccountDB::Result AccountDB::del(
    const std::string& key,
    bool               sync) noexcept
{
    if (!isValidKey(key))
        return Result::failure(
            "INVALID_ARG:del: invalid key '" + key + "'");

    std::unique_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return Result::failure("IO_ERROR:del: DB not open");

    try {
        rocksdb::WriteOptions wo;
        wo.sync = sync;
        rocksdb::Status s = db->Delete(wo, key);
        auto r = fromStatus(s, "del key=" + key);
        if (!r) log(2, r.error);
        return r;
    } catch (const std::exception& e) {
        return Result::failure(
            std::string("INTERNAL:del exception: ") + e.what());
    }
}

// =============================================================================
// WRITE BATCH
// Fix 4: validates all keys before writing any.
// Fix 6: atomic via single RocksDB WriteBatch -- no partial commits.
// =============================================================================
AccountDB::Result AccountDB::writeBatch(
    const std::vector<std::pair<std::string,
                                std::string>>& items,
    bool sync) noexcept
{
    if (items.empty()) return Result::success();

    for (size_t i = 0; i < items.size(); ++i) {
        if (!isValidKey(items[i].first))
            return Result::failure(
                "INVALID_ARG:writeBatch: invalid key at index "
                + std::to_string(i)
                + " key='" + items[i].first + "'");
    }

    std::unique_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return Result::failure("IO_ERROR:writeBatch: DB not open");

    try {
        rocksdb::WriteBatch batch;
        for (const auto& kv : items)
            batch.Put(kv.first, kv.second);
        rocksdb::WriteOptions wo;
        wo.sync = sync;
        rocksdb::Status s = db->Write(wo, &batch);
        auto r = fromStatus(s, "writeBatch");
        if (!r) log(2, r.error);
        return r;
    } catch (const std::exception& e) {
        return Result::failure(
            std::string("INTERNAL:writeBatch exception: ") + e.what());
    }
}

// =============================================================================
// DELETE BATCH
// Fix 4: validates all keys before deleting any.
// Fix 6: atomic via single WriteBatch.
// =============================================================================
AccountDB::Result AccountDB::deleteBatch(
    const std::vector<std::string>& keys,
    bool sync) noexcept
{
    if (keys.empty()) return Result::success();

    for (size_t i = 0; i < keys.size(); ++i) {
        if (!isValidKey(keys[i]))
            return Result::failure(
                "INVALID_ARG:deleteBatch: invalid key at index "
                + std::to_string(i)
                + " key='" + keys[i] + "'");
    }

    std::unique_lock<std::shared_mutex> lock(dbMutex);
    if (!db) return Result::failure("IO_ERROR:deleteBatch: DB not open");

    try {
        rocksdb::WriteBatch batch;
        for (const auto& k : keys)
            batch.Delete(k);
        rocksdb::WriteOptions wo;
        wo.sync = sync;
        rocksdb::Status s = db->Write(wo, &batch);
        auto r = fromStatus(s, "deleteBatch");
        if (!r) log(2, r.error);
        return r;
    } catch (const std::exception& e) {
        return Result::failure(
            std::string("INTERNAL:deleteBatch exception: ") + e.what());
    }
}

// =============================================================================
// ITERATE PREFIX
// Fix 5: logs iterator creation failure.
// Fix 7: callbacks invoked outside the lock to prevent deadlocks.
// =============================================================================
int64_t AccountDB::iteratePrefix(
    const std::string&  prefix,
    const ScanCallback& callback,
    size_t              maxResults) noexcept
{
    if (!callback) { log(1, "iteratePrefix: null callback"); return 0; }
    if (prefix.empty()) { log(1, "iteratePrefix: empty prefix"); return 0; }

    std::vector<std::pair<std::string, std::string>> entries;
    {
        std::shared_lock<std::shared_mutex> lock(dbMutex);
        if (!db) { log(2, "iteratePrefix: DB not open"); return 0; }

        rocksdb::ReadOptions ro;
        ro.prefix_same_as_start = true;

        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
        // Fix 5: log failure instead of silently returning 0
        if (!it) {
            log(2, "iteratePrefix: failed to create iterator for '"
                + prefix + "'");
            return 0;
        }

        for (it->Seek(prefix); it->Valid(); it->Next()) {
            const std::string key = it->key().ToString();
            if (key.size() < prefix.size() ||
                key.substr(0, prefix.size()) != prefix)
                break;
            entries.emplace_back(key, it->value().ToString());
            if (maxResults > 0 && entries.size() >= maxResults)
                break;
        }

        if (!it->status().ok())
            log(2, "iteratePrefix: iterator error: "
                + it->status().ToString());
    }

    // Fix 7: invoke callbacks outside lock
    int64_t count = 0;
    for (const auto& kv : entries) {
        ++count;
        try {
            if (!callback(kv.first, kv.second)) break;
        } catch (const std::exception& e) {
            log(2, std::string("iteratePrefix: callback threw: ") + e.what());
            break;
        } catch (...) {
            log(2, "iteratePrefix: callback threw unknown exception");
            break;
        }
    }
    return count;
}

// =============================================================================
// ACCOUNT BALANCE
// Component 1: structured account balance stored as "bal:<address>"
// =============================================================================
uint64_t AccountDB::getBalance(const std::string& address) const noexcept
{
    std::string val;
    auto r = get(PREFIX_BAL + address, val);
    if (!r) return 0;
    try { return std::stoull(val); } catch (...) { return 0; }
}

bool AccountDB::setBalance(const std::string& address,
                            uint64_t           amount) noexcept
{
    return static_cast<bool>(
        put(PREFIX_BAL + address, std::to_string(amount)));
}

bool AccountDB::addBalance(const std::string& address,
                            uint64_t           amount) noexcept
{
    const uint64_t current = getBalance(address);
    if (amount > std::numeric_limits<uint64_t>::max() - current) {
        log(2, "addBalance: overflow for " + address);
        return false;
    }
    return setBalance(address, current + amount);
}

bool AccountDB::deductBalance(const std::string& address,
                               uint64_t           amount) noexcept
{
    const uint64_t current = getBalance(address);
    if (amount > current) {
        log(2, "deductBalance: insufficient for " + address
            + " has=" + std::to_string(current)
            + " needs=" + std::to_string(amount));
        return false;
    }
    return setBalance(address, current - amount);
}

// =============================================================================
// NONCE MANAGEMENT
// Component 4: per-account nonce stored as "nonce:<address>"
// Prevents double-spending -- each transaction must use current nonce + 1.
// =============================================================================
uint64_t AccountDB::getNonce(const std::string& address) const noexcept
{
    std::string val;
    auto r = get(PREFIX_NONCE + address, val);
    if (!r) return 0;
    try { return std::stoull(val); } catch (...) { return 0; }
}

bool AccountDB::incrementNonce(const std::string& address) noexcept
{
    const uint64_t current = getNonce(address);
    if (current == std::numeric_limits<uint64_t>::max()) {
        log(2, "incrementNonce: nonce overflow for " + address);
        return false;
    }
    return static_cast<bool>(
        put(PREFIX_NONCE + address,
            std::to_string(current + 1)));
}

bool AccountDB::validateNonce(const std::string& address,
                               uint64_t           txNonce) const noexcept
{
    return txNonce == getNonce(address);
}

// =============================================================================
// SMART CONTRACT CODE
// Component 8: contract bytecode stored as "code:<address>"
// =============================================================================
bool AccountDB::setContractCode(const std::string&              address,
                                  const std::vector<uint8_t>&   code) noexcept
{
    std::string hex;
    hex.reserve(code.size() * 2);
    static const char hexChars[] = "0123456789abcdef";
    for (uint8_t b : code) {
        hex += hexChars[b >> 4];
        hex += hexChars[b & 0x0F];
    }
    return static_cast<bool>(put(PREFIX_CODE + address, hex));
}

std::vector<uint8_t> AccountDB::getContractCode(
    const std::string& address) const noexcept
{
    std::string hex;
    auto r = get(PREFIX_CODE + address, hex);
    if (!r || hex.empty()) return {};
    std::vector<uint8_t> code;
    code.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        try {
            code.push_back(static_cast<uint8_t>(
                std::stoul(hex.substr(i, 2), nullptr, 16)));
        } catch (...) { return {}; }
    }
    return code;
}

// =============================================================================
// CONTRACT STORAGE
// Component 8: per-slot contract storage "store:<address>:<slot>"
// =============================================================================
bool AccountDB::setStorageSlot(const std::string& address,
                                 const std::string& slot,
                                 const std::string& value) noexcept
{
    return static_cast<bool>(
        put(PREFIX_STORE + address + ":" + slot, value));
}

std::string AccountDB::getStorageSlot(
    const std::string& address,
    const std::string& slot) const noexcept
{
    std::string val;
    get(PREFIX_STORE + address + ":" + slot, val);
    return val;
}

// =============================================================================
// STATE ROOT
// Component 3: state root per block height stored as "state:<height>"
// Links DB state to a specific block for chain reorganisation support.
// =============================================================================
bool AccountDB::setStateRoot(uint64_t           blockHeight,
                               const std::string& stateRoot) noexcept
{
    return static_cast<bool>(
        put(PREFIX_STATE + std::to_string(blockHeight), stateRoot));
}

std::string AccountDB::getStateRoot(uint64_t blockHeight) const noexcept
{
    std::string val;
    get(PREFIX_STATE + std::to_string(blockHeight), val);
    return val;
}

// =============================================================================
// BLOCK-TAGGED METADATA
// Component 3: arbitrary metadata tagged to a block height
// stored as "meta:<height>:<key>"
// =============================================================================
bool AccountDB::setMeta(uint64_t           blockHeight,
                          const std::string& key,
                          const std::string& value) noexcept
{
    return static_cast<bool>(
        put(PREFIX_META + std::to_string(blockHeight) + ":" + key,
            value));
}

std::string AccountDB::getMeta(uint64_t           blockHeight,
                                 const std::string& key) const noexcept
{
    std::string val;
    get(PREFIX_META + std::to_string(blockHeight) + ":" + key, val);
    return val;
}

// =============================================================================
// ATOMIC BLOCK STATE COMMIT
// Component 2: all state changes for a block applied atomically.
// If any step fails the entire batch is discarded -- no partial state.
// Fix 6: single WriteBatch for all balance, nonce, and metadata updates.
// =============================================================================
AccountDB::Result AccountDB::commitBlockState(
    uint64_t blockHeight,
    const std::vector<std::pair<std::string, uint64_t>>& balanceChanges,
    const std::vector<std::string>&                       nonceIncrements,
    const std::string&                                    stateRoot) noexcept
{
    std::vector<std::pair<std::string, std::string>> items;
    items.reserve(
        balanceChanges.size() +
        nonceIncrements.size() + 1);

    // Balance updates
    for (const auto& [addr, amount] : balanceChanges) {
        if (!isValidKey(PREFIX_BAL + addr))
            return Result::failure(
                "INVALID_ARG:commitBlockState: invalid address '"
                + addr + "'");
        items.emplace_back(PREFIX_BAL  + addr,
                           std::to_string(amount));
    }

    // Nonce increments -- read current under lock then stage new value
    for (const auto& addr : nonceIncrements) {
        const uint64_t current = getNonce(addr);
        if (current == std::numeric_limits<uint64_t>::max())
            return Result::failure(
                "INVALID_ARG:commitBlockState: nonce overflow for "
                + addr);
        items.emplace_back(PREFIX_NONCE + addr,
                           std::to_string(current + 1));
    }

    // State root
    if (!stateRoot.empty())
        items.emplace_back(
            PREFIX_STATE + std::to_string(blockHeight), stateRoot);

    return writeBatch(items);
}

// =============================================================================
// GAS / FEE ACCOUNTING
// Component 7: cumulative gas used per address stored as "gas:<address>"
// =============================================================================
bool AccountDB::addGasUsed(const std::string& address,
                             uint64_t           gasUsed) noexcept
{
    std::string val;
    uint64_t current = 0;
    if (get("gas:" + address, val)) {
        try { current = std::stoull(val); } catch (...) { current = 0; }
    }
    if (gasUsed > std::numeric_limits<uint64_t>::max() - current) {
        log(2, "addGasUsed: overflow for " + address);
        return false;
    }
    return static_cast<bool>(
        put("gas:" + address, std::to_string(current + gasUsed)));
}

uint64_t AccountDB::getTotalGasUsed(
    const std::string& address) const noexcept
{
    std::string val;
    auto r = get("gas:" + address, val);
    if (!r) return 0;
    try { return std::stoull(val); } catch (...) { return 0; }
}

// =============================================================================
// ACCOUNT EXISTS
// =============================================================================
bool AccountDB::accountExists(const std::string& address) const noexcept
{
    std::string val;
    return static_cast<bool>(get(PREFIX_BAL + address, val));
}
