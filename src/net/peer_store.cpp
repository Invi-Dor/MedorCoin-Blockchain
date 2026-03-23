#include "net/peer_store.h"

#include <iostream>
#include <shared_mutex>
#include <stdexcept>

// =============================================================================
// CONSTRUCTOR
// Fix 1: opens RocksDB once, assigns raw pointer only on success.
// =============================================================================
PeerStore::PeerStore(const std::string &dbPath)
{
    if (dbPath.empty()) {
        log(2, "FATAL: empty dbPath rejected");
        return;
    }

    opts_.create_if_missing        = true;
    opts_.paranoid_checks          = true;
    opts_.compression              = rocksdb::kSnappyCompression;
    opts_.write_buffer_size        = 16 * 1024 * 1024;
    opts_.max_open_files           = 256;

    std::unique_ptr<rocksdb::DB> raw;
    rocksdb::Status s = rocksdb::DB::Open(opts_, dbPath, &raw);
    if (!s.ok()) {
        // Fix 2: log the full RocksDB error string
        log(2, "Failed to open at '" + dbPath
            + "': " + s.ToString());
        return;
    }

    db_ = raw.release();
    log(0, "Opened at '" + dbPath + "'");
}

// =============================================================================
// DESTRUCTOR
// Fix 1: db_ deleted exactly once -- no double-free possible because
// db_ is set to nullptr immediately after delete.
// =============================================================================
PeerStore::~PeerStore()
{
    if (db_) {
        // Flush WAL before closing to prevent data loss
        db_->FlushWAL(true);
        delete db_;
        db_ = nullptr;
        log(0, "Closed");
    }
}

bool PeerStore::isOpen() const noexcept
{
    std::shared_lock<std::shared_mutex> lk(mu_);
    return db_ != nullptr;
}

// =============================================================================
// LOGGING
// Fix 2: consistent logging with level and context
// =============================================================================
void PeerStore::log(int level, const std::string &msg) const noexcept
{
    if (level >= 2)
        std::cerr << "[PeerStore][ERROR] " << msg << "\n";
    else if (level == 1)
        std::cerr << "[PeerStore][WARN]  " << msg << "\n";
    else
        std::cout << "[PeerStore][INFO]  " << msg << "\n";
}

// =============================================================================
// SERIALIZATION
// Fix 3: all fields serialized explicitly by name
// Fix 5: schemaVersion embedded in every record
// =============================================================================
nlohmann::json PeerStore::peerToJson(
    const PersistedPeer &peer) noexcept
{
    try {
        return nlohmann::json{
            {"schemaVersion", PEER_SCHEMA_VERSION},
            {"id",            peer.id},
            {"host",          peer.host},
            {"port",          peer.port},
            {"trusted",       peer.trusted},
            {"priority",      peer.priority},
            {"region",        peer.region},
            {"tags",          peer.tags},
            {"banCount",      peer.banCount},
            {"bannedUntil",   peer.bannedUntil},
            {"score",         peer.score},
            {"lastSeen",      peer.lastSeen},
            {"bytesReceived", peer.bytesReceived},
            {"bytesSent",     peer.bytesSent},
            {"blocksRelayed", peer.blocksRelayed}
        };
    } catch (...) {
        return {};
    }
}

// =============================================================================
// DESERIALIZATION
// Fix 5: schema version checked -- old records handled gracefully
// Fix 6: every field validated before use -- no silent corruption
// =============================================================================
bool PeerStore::jsonToPeer(const nlohmann::json &j,
                             PersistedPeer        &out) noexcept
{
    try {
        // Fix 6: validate required fields exist and have correct types
        if (!j.is_object())
            return false;

        // Fix 5: check schema version -- skip incompatible records
        if (!j.contains("schemaVersion")
         || !j["schemaVersion"].is_number_unsigned())
            return false;

        uint32_t sv = j["schemaVersion"].get<uint32_t>();
        if (sv > PEER_SCHEMA_VERSION) {
            // Record from a newer node -- skip gracefully
            return false;
        }

        // Fix 6: validate all required fields
        if (!j.contains("id")     || !j["id"].is_string())     return false;
        if (!j.contains("host")   || !j["host"].is_string())   return false;
        if (!j.contains("port")   || !j["port"].is_number())   return false;
        if (!j.contains("trusted")|| !j["trusted"].is_boolean())return false;

        out.schemaVersion = sv;
        out.id            = j["id"].get<std::string>();
        out.host          = j["host"].get<std::string>();
        out.port          = j["port"].get<int>();
        out.trusted       = j["trusted"].get<bool>();

        // Fix 6: optional fields with safe defaults
        out.priority      = j.value("priority",     2);
        out.region        = j.value("region",        std::string{});
        out.banCount      = j.value("banCount",      0);
        out.bannedUntil   = j.value("bannedUntil",   uint64_t{0});
        out.score         = j.value("score",         1.0);
        out.lastSeen      = j.value("lastSeen",      uint64_t{0});
        out.bytesReceived = j.value("bytesReceived", uint64_t{0});
        out.bytesSent     = j.value("bytesSent",     uint64_t{0});
        out.blocksRelayed = j.value("blocksRelayed", uint64_t{0});

        if (j.contains("tags") && j["tags"].is_array()) {
            out.tags.clear();
            for (const auto &t : j["tags"]) {
                if (t.is_string())
                    out.tags.push_back(t.get<std::string>());
            }
        }

        // Fix 6: validate port range
        if (out.port < 1 || out.port > 65535)
            return false;

        // Fix 6: validate id and host not empty
        if (out.id.empty() || out.host.empty())
            return false;

        return true;

    } catch (const std::exception &e) {
        return false;
    } catch (...) {
        return false;
    }
}

nlohmann::json PeerStore::banToJson(uint64_t bannedUntil,
                                     int      banCount) noexcept
{
    try {
        return nlohmann::json{
            {"schemaVersion", BAN_SCHEMA_VERSION},
            {"bannedUntil",   bannedUntil},
            {"banCount",      banCount}
        };
    } catch (...) {
        return {};
    }
}

// =============================================================================
// SAVE PEER
// Fix 4: unique_lock for writes only -- reads use shared_lock
// =============================================================================
bool PeerStore::savePeer(const PersistedPeer &peer) noexcept
{
    // Fix 6: validate before writing
    if (peer.id.empty() || peer.host.empty()) {
        log(1, "savePeer: rejected empty id or host");
        return false;
    }
    if (peer.port < 1 || peer.port > 65535) {
        log(1, "savePeer: rejected invalid port "
            + std::to_string(peer.port));
        return false;
    }

    nlohmann::json j = peerToJson(peer);
    if (j.empty()) {
        log(2, "savePeer: serialization failed for " + peer.id);
        return false;
    }

    std::string serialized;
    try { serialized = j.dump(); }
    catch (const std::exception &e) {
        log(2, "savePeer: dump failed: " + std::string(e.what()));
        return false;
    }

    // Fix 4: unique_lock for write
    std::unique_lock<std::shared_mutex> lk(mu_);
    if (!db_) {
        log(2, "savePeer: DB not open");
        return false;
    }

    rocksdb::WriteOptions wo; wo.sync = true;
    rocksdb::Status s = db_->Put(wo, PREFIX_PEER + peer.id, serialized);
    if (!s.ok()) {
        // Fix 2: log full RocksDB error
        log(2, "savePeer: RocksDB Put failed for "
            + peer.id + ": " + s.ToString());
        return false;
    }
    return true;
}

// =============================================================================
// LOAD PEER
// Fix 4: shared_lock for reads
// =============================================================================
bool PeerStore::loadPeer(const std::string &id,
                          PersistedPeer     &out) noexcept
{
    if (id.empty()) return false;

    // Fix 4: shared_lock for read -- does not block other reads
    std::shared_lock<std::shared_mutex> lk(mu_);
    if (!db_) return false;

    std::string raw;
    rocksdb::Status s = db_->Get(
        rocksdb::ReadOptions(), PREFIX_PEER + id, &raw);
    if (s.IsNotFound()) return false;
    if (!s.ok()) {
        // Fix 2: log RocksDB error
        log(2, "loadPeer: RocksDB Get failed for "
            + id + ": " + s.ToString());
        return false;
    }

    // Fix 6: validate before parsing
    if (raw.empty()) {
        log(1, "loadPeer: empty record for " + id);
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(raw);
    } catch (const std::exception &e) {
        // Fix 6: corrupt record handled gracefully
        log(2, "loadPeer: JSON parse failed for "
            + id + ": " + e.what());
        return false;
    }

    return jsonToPeer(j, out);
}

// =============================================================================
// DELETE PEER
// =============================================================================
bool PeerStore::deletePeer(const std::string &id) noexcept
{
    if (id.empty()) return false;

    std::unique_lock<std::shared_mutex> lk(mu_);
    if (!db_) return false;

    rocksdb::WriteOptions wo; wo.sync = true;
    rocksdb::Status s = db_->Delete(wo, PREFIX_PEER + id);
    if (!s.ok() && !s.IsNotFound()) {
        log(2, "deletePeer: failed for "
            + id + ": " + s.ToString());
        return false;
    }
    return true;
}

// =============================================================================
// LOAD ALL PEERS
// Fix 6: each record individually validated -- corrupt records skipped
// Fix 4: shared_lock for full scan
// =============================================================================
std::vector<PersistedPeer> PeerStore::loadAllPeers() noexcept
{
    std::vector<PersistedPeer> result;

    std::shared_lock<std::shared_mutex> lk(mu_);
    if (!db_) return result;

    try {
        std::unique_ptr<rocksdb::Iterator> it(
            db_->NewIterator(rocksdb::ReadOptions()));

        std::string prefix = PREFIX_PEER;
        for (it->Seek(prefix); it->Valid(); it->Next()) {
            std::string key = it->key().ToString();
            if (key.size() < prefix.size() ||
                key.substr(0, prefix.size()) != prefix)
                break;

            // Fix 6: skip corrupt records individually
            nlohmann::json j;
            try {
                j = nlohmann::json::parse(
                    it->value().ToString());
            } catch (...) {
                log(1, "loadAllPeers: skipping corrupt record "
                    + key);
                continue;
            }

            PersistedPeer peer;
            if (jsonToPeer(j, peer)) {
                result.push_back(std::move(peer));
            } else {
                log(1, "loadAllPeers: skipping invalid record "
                    + key);
            }
        }

        if (!it->status().ok()) {
            log(2, "loadAllPeers: iterator error: "
                + it->status().ToString());
        }

    } catch (const std::exception &e) {
        log(2, "loadAllPeers: exception: "
            + std::string(e.what()));
    }

    return result;
}

// =============================================================================
// SAVE BAN
// =============================================================================
bool PeerStore::saveBan(const std::string &id,
                         uint64_t           bannedUntil,
                         int                banCount) noexcept
{
    if (id.empty()) return false;

    nlohmann::json j = banToJson(bannedUntil, banCount);
    std::string serialized;
    try { serialized = j.dump(); }
    catch (...) { return false; }

    std::unique_lock<std::shared_mutex> lk(mu_);
    if (!db_) return false;

    rocksdb::WriteOptions wo; wo.sync = true;
    rocksdb::Status s = db_->Put(wo, PREFIX_BAN + id, serialized);
    if (!s.ok()) {
        log(2, "saveBan: failed for "
            + id + ": " + s.ToString());
        return false;
    }
    return true;
}

// =============================================================================
// LOAD BAN
// Fix 6: validates schema version and field types
// =============================================================================
bool PeerStore::loadBan(const std::string &id,
                         uint64_t          &bannedUntil,
                         int               &banCount) noexcept
{
    if (id.empty()) return false;

    std::shared_lock<std::shared_mutex> lk(mu_);
    if (!db_) return false;

    std::string raw;
    rocksdb::Status s = db_->Get(
        rocksdb::ReadOptions(), PREFIX_BAN + id, &raw);
    if (s.IsNotFound()) return false;
    if (!s.ok()) {
        log(2, "loadBan: RocksDB error for "
            + id + ": " + s.ToString());
        return false;
    }

    try {
        auto j = nlohmann::json::parse(raw);

        // Fix 6: validate required fields
        if (!j.is_object())                            return false;
        if (!j.contains("bannedUntil"))                return false;
        if (!j.contains("banCount"))                   return false;
        if (!j["bannedUntil"].is_number_unsigned())    return false;
        if (!j["banCount"].is_number())                return false;

        // Fix 5: check schema version
        if (j.contains("schemaVersion")) {
            uint32_t sv = j["schemaVersion"].get<uint32_t>();
            if (sv > BAN_SCHEMA_VERSION) return false;
        }

        bannedUntil = j["bannedUntil"].get<uint64_t>();
        banCount    = j["banCount"].get<int>();
        return true;

    } catch (const std::exception &e) {
        log(2, "loadBan: parse failed for "
            + id + ": " + e.what());
        return false;
    }
}

// =============================================================================
// CLEAR BAN
// =============================================================================
bool PeerStore::clearBan(const std::string &id) noexcept
{
    if (id.empty()) return false;

    std::unique_lock<std::shared_mutex> lk(mu_);
    if (!db_) return false;

    rocksdb::WriteOptions wo; wo.sync = true;
    rocksdb::Status s = db_->Delete(wo, PREFIX_BAN + id);
    if (!s.ok() && !s.IsNotFound()) {
        log(2, "clearBan: failed for "
            + id + ": " + s.ToString());
        return false;
    }
    return true;
}

// =============================================================================
// LOAD ALL BANS
// Fix 6: each record individually validated
// =============================================================================
std::unordered_map<std::string, uint64_t>
PeerStore::loadAllBans() noexcept
{
    std::unordered_map<std::string, uint64_t> result;

    std::shared_lock<std::shared_mutex> lk(mu_);
    if (!db_) return result;

    try {
        std::unique_ptr<rocksdb::Iterator> it(
            db_->NewIterator(rocksdb::ReadOptions()));

        std::string prefix = PREFIX_BAN;
        for (it->Seek(prefix); it->Valid(); it->Next()) {
            std::string key = it->key().ToString();
            if (key.size() < prefix.size() ||
                key.substr(0, prefix.size()) != prefix)
                break;

            try {
                auto j = nlohmann::json::parse(
                    it->value().ToString());

                // Fix 6: validate fields
                if (!j.contains("bannedUntil"))     continue;
                if (!j["bannedUntil"].is_number())  continue;

                std::string id = key.substr(prefix.size());
                if (id.empty()) continue;

                result[id] = j["bannedUntil"].get<uint64_t>();

            } catch (...) {
                log(1, "loadAllBans: skipping corrupt record "
                    + key);
            }
        }

        if (!it->status().ok()) {
            log(2, "loadAllBans: iterator error: "
                + it->status().ToString());
        }

    } catch (const std::exception &e) {
        log(2, "loadAllBans: exception: "
            + std::string(e.what()));
    }

    return result;
}

// =============================================================================
// SAVE METRICS
// Fix 4: unique_lock for write -- sync=false for metrics (not critical)
// =============================================================================
bool PeerStore::saveMetrics(const nlohmann::json &metrics) noexcept
{
    std::string serialized;
    try { serialized = metrics.dump(); }
    catch (...) { return false; }

    std::unique_lock<std::shared_mutex> lk(mu_);
    if (!db_) return false;

    rocksdb::WriteOptions wo; wo.sync = false;
    rocksdb::Status s = db_->Put(wo, KEY_METRICS, serialized);
    if (!s.ok()) {
        log(1, "saveMetrics: failed: " + s.ToString());
        return false;
    }
    return true;
}

// =============================================================================
// LOAD METRICS
// Fix 6: validates JSON before returning
// =============================================================================
bool PeerStore::loadMetrics(nlohmann::json &out) noexcept
{
    std::shared_lock<std::shared_mutex> lk(mu_);
    if (!db_) return false;

    std::string raw;
    rocksdb::Status s = db_->Get(
        rocksdb::ReadOptions(), KEY_METRICS, &raw);
    if (s.IsNotFound()) return false;
    if (!s.ok()) {
        log(2, "loadMetrics: RocksDB error: " + s.ToString());
        return false;
    }

    // Fix 6: validate before returning
    try {
        out = nlohmann::json::parse(raw);
        if (!out.is_object()) {
            log(2, "loadMetrics: parsed value is not an object");
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        log(2, "loadMetrics: parse failed: "
            + std::string(e.what()));
        return false;
    }
}
