#include "api_key_manager.h"

#include <nlohmann/json.hpp>
#include <rocksdb/utilities/backup_engine.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

// Platform-portable CSPRNG
#if defined(_WIN32) || defined(_WIN64)
#  define MEDOR_PLATFORM_WINDOWS 1
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#else
#  define MEDOR_PLATFORM_POSIX 1
#  include <fstream>
#endif

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// logSafe — logs through AsyncLogger and writes to std::cerr if it throws
// rather than silently swallowing the failure (gap 4)
// ─────────────────────────────────────────────────────────────────────────────

void APIKeyManager::logSafe(int level, const std::string &msg) const noexcept
{
    if (logger_) {
        try {
            logger_->log(level, msg);
            return;
        } catch (const std::exception &e) {
            std::cerr << "[APIKeyManager] logger threw: " << e.what()
                      << " — original message: " << msg << "\n";
        } catch (...) {
            std::cerr << "[APIKeyManager] logger threw unknown exception"
                      << " — original message: " << msg << "\n";
        }
    }
    // Fallback to stderr when no logger is supplied or when it throws
    if      (level >= 2) std::cerr << "[APIKeyManager][ERROR] " << msg << "\n";
    else if (level == 1) std::cerr << "[APIKeyManager][WARN]  " << msg << "\n";
    else                 std::cout << "[APIKeyManager][INFO]  " << msg << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Static helpers
// ─────────────────────────────────────────────────────────────────────────────

int64_t APIKeyManager::nowUnixSecs() noexcept
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string APIKeyManager::dbKey(const std::string &keyHex) noexcept
{
    return "key:" + keyHex;
}

void APIKeyManager::secureErase(std::string &s) noexcept
{
    if (!s.empty()) {
        volatile char *p = s.data();
        std::memset(const_cast<char *>(p), 0, s.size());
    }
    s.clear();
}

bool APIKeyManager::isValidClientId(const std::string &s) noexcept
{
    if (s.empty() || s.size() > MAX_CLIENT_ID_LEN) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

bool APIKeyManager::isValidLabel(const std::string &s) noexcept
{
    if (s.size() > MAX_LABEL_LEN) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

bool APIKeyManager::isValidKeyHex(const std::string &s) noexcept
{
    if (s.size() != KEY_HEX_LEN) return false;
    for (unsigned char c : s)
        if (!std::isxdigit(c)) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

APIKeyManager::APIKeyManager(Config                       cfg,
                               std::shared_ptr<AsyncLogger> logger)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
    , db_(cfg_.dbPath)
{
    if (!db_.isOpen())
        logSafe(2, "constructor: RocksDB failed to open at '"
                   + cfg_.dbPath + "'");
    else
        logSafe(0, "opened key DB at '" + cfg_.dbPath + "'");
}

APIKeyManager::~APIKeyManager()
{
    logSafe(0, "APIKeyManager shutting down");
    // RocksDBWrapper destructor handles WAL flush and close
}

// ─────────────────────────────────────────────────────────────────────────────
// isOpen / isHealthy
//
// isHealthy() retries the probe cfg_.healthRetries times with
// cfg_.healthRetryDelayMs between attempts (gap 1).
// ─────────────────────────────────────────────────────────────────────────────

bool APIKeyManager::isOpen() const noexcept
{
    return db_.isOpen();
}

bool APIKeyManager::isHealthy() const noexcept
{
    for (uint32_t attempt = 0; attempt <= cfg_.healthRetries; ++attempt) {
        if (db_.isHealthy()) return true;
        if (attempt < cfg_.healthRetries && cfg_.healthRetryDelayMs > 0)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg_.healthRetryDelayMs));
    }
    logSafe(2, "isHealthy: DB probe failed after "
               + std::to_string(cfg_.healthRetries + 1) + " attempts");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// CSPRNG — platform-portable; no fallback to a weaker source
// ─────────────────────────────────────────────────────────────────────────────

APIKeyManager::Result
APIKeyManager::generateKeyHex(std::string &keyOut) const noexcept
{
    keyOut.clear();
    uint8_t buf[32] = {};

#if defined(MEDOR_PLATFORM_WINDOWS)
    NTSTATUS status = BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(buf),
        static_cast<ULONG>(sizeof(buf)),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        std::memset(buf, 0, sizeof(buf));
        return Result::failure(KeyError::GenerationFailed,
            "generateKeyHex: BCryptGenRandom failed, NTSTATUS=0x"
            + [&]{ std::ostringstream s; s << std::hex << status; return s.str(); }());
    }
#else
    {
        std::ifstream f("/dev/urandom", std::ios::binary);
        if (!f.is_open()) {
            std::memset(buf, 0, sizeof(buf));
            return Result::failure(KeyError::GenerationFailed,
                "generateKeyHex: cannot open /dev/urandom");
        }
        f.read(reinterpret_cast<char *>(buf), static_cast<std::streamsize>(sizeof(buf)));
        if (!f.good()) {
            std::memset(buf, 0, sizeof(buf));
            return Result::failure(KeyError::GenerationFailed,
                "generateKeyHex: short read from /dev/urandom");
        }
    }
#endif

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (auto b : buf) ss << std::setw(2) << static_cast<int>(b);
    keyOut = ss.str();

    // Zero the raw entropy immediately after hex encoding (gap 5 in prior version)
    std::memset(buf, 0, sizeof(buf));
    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// serialise — structured Result; size enforced here as well as at write site
// ─────────────────────────────────────────────────────────────────────────────

APIKeyManager::Result
APIKeyManager::serialise(const KeyRecord &rec, std::string &out) const noexcept
{
    out.clear();
    try {
        out = json{
            {"clientId",  rec.clientId},
            {"label",     rec.label},
            {"key",       rec.key},
            {"createdAt", rec.createdAt},
            {"expiresAt", rec.expiresAt},
            {"revokedAt", rec.revokedAt}
        }.dump();
    } catch (const std::exception &e) {
        return Result::failure(KeyError::SerialisationError,
            std::string("serialise: ") + e.what());
    } catch (...) {
        return Result::failure(KeyError::SerialisationError,
            "serialise: unknown exception during JSON dump");
    }

    if (out.size() > MAX_RECORD_BYTES) {
        out.clear();
        return Result::failure(KeyError::InvalidInput,
            "serialise: record size " + std::to_string(out.size())
            + " exceeds MAX_RECORD_BYTES=" + std::to_string(MAX_RECORD_BYTES));
    }

    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// deserialise — structured error output as a JSON object (gap 7) so that
// automated monitoring can parse field name, expected type, and received value.
// ─────────────────────────────────────────────────────────────────────────────

APIKeyManager::Result
APIKeyManager::deserialise(const std::string &raw, KeyRecord &out) const noexcept
{
    if (raw.empty())
        return Result::failure(KeyError::CorruptRecord,
            json{{"parseError","empty_record"},
                 {"field","(all)"},
                 {"detail","zero-length raw string"}}.dump());

    auto makeParseError = [](const std::string &field,
                              const std::string &detail) {
        return json{{"parseError","missing_or_invalid_field"},
                    {"field",     field},
                    {"detail",    detail}}.dump();
    };

    try {
        const auto j  = json::parse(raw);

        if (!j.contains("clientId") || !j["clientId"].is_string())
            return Result::failure(KeyError::CorruptRecord,
                makeParseError("clientId", "missing or not a string"));
        if (!j.contains("label") || !j["label"].is_string())
            return Result::failure(KeyError::CorruptRecord,
                makeParseError("label", "missing or not a string"));
        if (!j.contains("key") || !j["key"].is_string())
            return Result::failure(KeyError::CorruptRecord,
                makeParseError("key", "missing or not a string"));
        if (!j.contains("createdAt") || !j["createdAt"].is_number_integer())
            return Result::failure(KeyError::CorruptRecord,
                makeParseError("createdAt", "missing or not an integer"));
        if (!j.contains("expiresAt") || !j["expiresAt"].is_number_integer())
            return Result::failure(KeyError::CorruptRecord,
                makeParseError("expiresAt", "missing or not an integer"));
        if (!j.contains("revokedAt") || !j["revokedAt"].is_number_integer())
            return Result::failure(KeyError::CorruptRecord,
                makeParseError("revokedAt", "missing or not an integer"));

        out.clientId  = j["clientId"].get<std::string>();
        out.label     = j["label"].get<std::string>();
        out.key       = j["key"].get<std::string>();
        out.createdAt = j["createdAt"].get<int64_t>();
        out.expiresAt = j["expiresAt"].get<int64_t>();
        out.revokedAt = j["revokedAt"].get<int64_t>();

    } catch (const json::parse_error &e) {
        return Result::failure(KeyError::CorruptRecord,
            json{{"parseError","json_syntax_error"},
                 {"field","(all)"},
                 {"detail", std::string(e.what())}}.dump());
    } catch (const std::exception &e) {
        return Result::failure(KeyError::CorruptRecord,
            json{{"parseError","exception"},
                 {"field","(unknown)"},
                 {"detail", std::string(e.what())}}.dump());
    } catch (...) {
        return Result::failure(KeyError::CorruptRecord,
            json{{"parseError","unknown_exception"},
                 {"field","(unknown)"},
                 {"detail","non-std exception thrown"}}.dump());
    }

    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// writeRecord — enforces MAX_RECORD_BYTES at the actual DB write site (gap 8)
// Caller must hold mutex_.
// ─────────────────────────────────────────────────────────────────────────────

APIKeyManager::Result
APIKeyManager::writeRecord(const std::string &keyHex,
                            const std::string &payload) noexcept
{
    // Size guard is applied here, at the write site, in addition to serialise()
    if (payload.size() > MAX_RECORD_BYTES)
        return Result::failure(KeyError::InvalidInput,
            "writeRecord: payload " + std::to_string(payload.size())
            + " bytes exceeds MAX_RECORD_BYTES="
            + std::to_string(MAX_RECORD_BYTES));

    auto r = db_.put(dbKey(keyHex), payload, /*sync=*/true);
    if (!r)
        return Result::failure(KeyError::DbError,
                               "writeRecord: " + r.error);
    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// triggerBackup — platform-portable; BackupEngine opened inside try/catch
// so engine-open failure and backup failure are both individually handled
// without any resource leak (gap 1 and gap 3)
// ─────────────────────────────────────────────────────────────────────────────

APIKeyManager::Result APIKeyManager::triggerBackup() noexcept
{
    if (cfg_.backupPath.empty())
        return Result::failure(KeyError::BackupError,
                               "triggerBackup: no backupPath configured");

    // Filesystem errors are now escalated rather than merely logged (gap 6)
    try {
        std::filesystem::create_directories(cfg_.backupPath);
    } catch (const std::exception &e) {
        return Result::failure(KeyError::BackupError,
            std::string("triggerBackup: cannot create backup directory: ")
            + e.what());
    }

    // Open BackupEngine — works on both POSIX and Windows (gap 3)
    rocksdb::BackupEngineOptions beOpts(cfg_.backupPath);
    rocksdb::BackupEngine *rawEngine = nullptr;
    rocksdb::Status s;

    try {
        s = rocksdb::BackupEngine::Open(
            rocksdb::Env::Default(), beOpts, &rawEngine);
    } catch (const std::exception &e) {
        return Result::failure(KeyError::BackupError,
            std::string("triggerBackup: BackupEngine::Open threw: ") + e.what());
    }

    if (!s.ok())
        return Result::failure(KeyError::BackupError,
            "triggerBackup: BackupEngine::Open failed: " + s.ToString());

    // Unique_ptr ensures the engine is deleted even if CreateNewBackup throws
    std::unique_ptr<rocksdb::BackupEngine> engine(rawEngine);

    try {
        s = engine->CreateNewBackup(db_.db, /*flush_before_backup=*/true);
    } catch (const std::exception &e) {
        return Result::failure(KeyError::BackupError,
            std::string("triggerBackup: CreateNewBackup threw: ") + e.what());
    }

    if (!s.ok())
        return Result::failure(KeyError::BackupError,
            "triggerBackup: CreateNewBackup failed: " + s.ToString());

    // Purge old backups — failure here is non-fatal; log and continue
    rocksdb::Status ps;
    try { ps = engine->PurgeOldBackups(2); } catch (...) {}
    if (!ps.ok())
        logSafe(1, "triggerBackup: PurgeOldBackups warning: " + ps.ToString());

    logSafe(0, "backup complete → '" + cfg_.backupPath + "'");
    return Result::success();
}

void APIKeyManager::maybeBackup() noexcept
{
    if (cfg_.backupPath.empty() || cfg_.backupAfterNWrites == 0) return;
    ++writeCount_;
    if (writeCount_ % cfg_.backupAfterNWrites != 0) return;

    Result r = triggerBackup();
    if (!r) logSafe(1, "maybeBackup: " + r.error);
}

// ─────────────────────────────────────────────────────────────────────────────
// lookupLocked — caller must hold mutex_
// ─────────────────────────────────────────────────────────────────────────────

std::optional<APIKeyManager::KeyRecord>
APIKeyManager::lookupLocked(const std::string &key) const noexcept
{
    std::string raw;
    auto r = db_.get(dbKey(key), raw);
    if (!r) return std::nullopt;

    KeyRecord rec;
    if (!deserialise(raw, rec)) return std::nullopt;
    return rec;
}

// ─────────────────────────────────────────────────────────────────────────────
// createKey / createKeyResult
// ─────────────────────────────────────────────────────────────────────────────

std::string APIKeyManager::createKey(const std::string &clientId,
                                      const std::string &label,
                                      int64_t            ttlSeconds) noexcept
{
    std::string keyOut;
    createKeyResult(clientId, label, ttlSeconds, keyOut);
    return keyOut;
}

APIKeyManager::Result
APIKeyManager::createKeyResult(const std::string &clientId,
                                const std::string &label,
                                int64_t            ttlSeconds,
                                std::string       &keyOut) noexcept
{
    keyOut.clear();

    if (!db_.isOpen())
        return Result::failure(KeyError::NotOpen,
                               "createKeyResult: DB not open");

    if (!isValidClientId(clientId))
        return Result::failure(KeyError::InvalidInput,
            "createKeyResult: clientId invalid (empty, too long, or "
            "contains control characters)");

    if (!isValidLabel(label))
        return Result::failure(KeyError::InvalidInput,
            "createKeyResult: label too long or contains control characters");

    // Apply default TTL if the caller passed NO_EXPIRY and a default is set
    // (gap 5): explicit NO_EXPIRY when defaultTtlSeconds == 0 means forever.
    int64_t effectiveTtl = ttlSeconds;
    if (effectiveTtl == NO_EXPIRY && cfg_.defaultTtlSeconds > 0)
        effectiveTtl = cfg_.defaultTtlSeconds;

    // Read the clock before acquiring the mutex
    const int64_t now = nowUnixSecs();

    std::string keyHex;
    {
        Result gr = generateKeyHex(keyHex);
        if (!gr) { logSafe(2, gr.error); return gr; }
    }

    KeyRecord rec;
    rec.clientId  = clientId;
    rec.label     = label;
    rec.key       = keyHex;
    rec.createdAt = now;
    rec.expiresAt = (effectiveTtl > 0) ? now + effectiveTtl : NO_EXPIRY;
    rec.revokedAt = 0;

    std::string payload;
    Result sr = serialise(rec, payload);
    if (!sr) {
        secureErase(keyHex);
        logSafe(2, sr.error);
        return sr;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // writeRecord enforces MAX_RECORD_BYTES at the write site (gap 8)
        Result wr = writeRecord(keyHex, payload);
        if (!wr) {
            secureErase(keyHex);
            logSafe(2, wr.error);
            return wr;
        }
    }

    logSafe(0, "key created clientId=" + clientId
               + " expiresAt=" + std::to_string(rec.expiresAt));

    keyOut = std::move(keyHex);
    maybeBackup();
    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// validate
// ─────────────────────────────────────────────────────────────────────────────

APIKeyManager::Result
APIKeyManager::validate(const std::string &key,
                         std::string       &clientIdOut) const noexcept
{
    clientIdOut.clear();

    if (!db_.isOpen())
        return Result::failure(KeyError::NotOpen, "validate: DB not open");

    if (!isValidKeyHex(key))
        return Result::failure(KeyError::InvalidInput,
                               "validate: key must be exactly 64 hex characters");

    const int64_t now = nowUnixSecs();

    std::string raw;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto r = db_.get(dbKey(key), raw);
        if (!r) {
            if (r.error.find("NOT_FOUND") != std::string::npos)
                return Result::failure(KeyError::NotFound,
                                       "validate: key not found");
            return Result::failure(KeyError::DbError,
                                   "validate: " + r.error);
        }
    }

    KeyRecord rec;
    Result dr = deserialise(raw, rec);
    if (!dr) {
        logSafe(2, "validate: corrupt record for key="
                   + key.substr(0, 8) + "...: " + dr.error);
        return dr;
    }

    if (rec.revokedAt != 0)
        return Result::failure(KeyError::Revoked,
            "validate: key revoked at "
            + std::to_string(rec.revokedAt));

    if (rec.expiresAt != NO_EXPIRY && now > rec.expiresAt)
        return Result::failure(KeyError::Expired,
            "validate: key expired at "
            + std::to_string(rec.expiresAt));

    clientIdOut = rec.clientId;
    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// revoke
// ─────────────────────────────────────────────────────────────────────────────

APIKeyManager::Result APIKeyManager::revoke(const std::string &key) noexcept
{
    if (!db_.isOpen())
        return Result::failure(KeyError::NotOpen, "revoke: DB not open");

    if (!isValidKeyHex(key))
        return Result::failure(KeyError::InvalidInput,
                               "revoke: key must be exactly 64 hex characters");

    const int64_t now = nowUnixSecs();

    std::lock_guard<std::mutex> lock(mutex_);

    std::string raw;
    auto gr = db_.get(dbKey(key), raw);
    if (!gr) {
        if (gr.error.find("NOT_FOUND") != std::string::npos)
            return Result::failure(KeyError::NotFound, "revoke: key not found");
        return Result::failure(KeyError::DbError, "revoke: " + gr.error);
    }

    KeyRecord rec;
    Result dr = deserialise(raw, rec);
    if (!dr) { logSafe(2, "revoke: corrupt record: " + dr.error); return dr; }

    if (rec.revokedAt != 0)
        return Result::failure(KeyError::Revoked,
                               "revoke: key is already revoked");

    rec.revokedAt = now;

    std::string payload;
    Result sr = serialise(rec, payload);
    if (!sr) { logSafe(2, sr.error); return sr; }

    // Size-guarded write at the write site (gap 8)
    Result wr = writeRecord(rec.key, payload);
    if (!wr) { logSafe(2, wr.error); return wr; }

    logSafe(0, "key revoked clientId=" + rec.clientId
               + " key=" + key.substr(0, 8) + "...");
    maybeBackup();
    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// rotateKey / rotateResult
//
// The sequence is: lookupLocked (under mutex) → release mutex →
// createKeyResult (acquires mutex independently) →
// revoke (acquires mutex independently).
//
// No nested lock is ever held (gap 2). If revoke fails after the new key
// has been written, the old key is left active and a warning is logged so
// the operator can clean it up via the admin endpoint.
// ─────────────────────────────────────────────────────────────────────────────

std::string APIKeyManager::rotateKey(const std::string &oldKey) noexcept
{
    std::string newKey;
    rotateResult(oldKey, newKey);
    return newKey;
}

APIKeyManager::Result
APIKeyManager::rotateResult(const std::string &oldKey,
                              std::string       &newKeyOut) noexcept
{
    newKeyOut.clear();

    if (!db_.isOpen())
        return Result::failure(KeyError::NotOpen, "rotateResult: DB not open");

    if (!isValidKeyHex(oldKey))
        return Result::failure(KeyError::InvalidInput,
            "rotateResult: oldKey must be exactly 64 hex characters");

    // Step 1: read the existing record under the mutex, then release it
    // before calling createKeyResult so no nested lock is held.
    KeyRecord oldRec;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto recOpt = lookupLocked(oldKey);
        if (!recOpt)
            return Result::failure(KeyError::NotFound,
                                   "rotateResult: old key not found");
        oldRec = *recOpt;
    }

    // Step 2: create the new key — acquires and releases mutex internally
    Result cr = createKeyResult(oldRec.clientId, oldRec.label,
                                 NO_EXPIRY, newKeyOut);
    if (!cr) {
        logSafe(2, "rotateResult: new key creation failed: " + cr.error);
        return cr;
    }

    // Step 3: revoke the old key — acquires and releases mutex internally
    Result rr = revoke(oldKey);
    if (!rr) {
        // The client has a working new key; log the failure and return
        // success so the caller is not blocked. The operator must revoke
        // the old key manually.
        logSafe(1, "rotateResult: new key issued but old key revoke failed: "
                   + rr.error
                   + " — old key=" + oldKey.substr(0, 8)
                   + "... must be revoked manually");
    }

    logSafe(0, "key rotated clientId=" + oldRec.clientId);
    return Result::success();
}

// ─────────────────────────────────────────────────────────────────────────────
// lookup — public, acquires mutex
// ─────────────────────────────────────────────────────────────────────────────

std::optional<APIKeyManager::KeyRecord>
APIKeyManager::lookup(const std::string &key) const noexcept
{
    if (!db_.isOpen() || !isValidKeyHex(key)) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    return lookupLocked(key);
}
