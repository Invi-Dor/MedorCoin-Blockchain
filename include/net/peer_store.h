#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

// =============================================================================
// PERSISTED PEER
// Schema version tracked inside every record so future field additions
// are handled gracefully without crashing old nodes on upgrade.
// Fix 5: PEER_SCHEMA_VERSION incremented whenever fields change.
// =============================================================================
static constexpr uint32_t PEER_SCHEMA_VERSION = 1;
static constexpr uint32_t BAN_SCHEMA_VERSION  = 1;

struct PersistedPeer {
    uint32_t                 schemaVersion = PEER_SCHEMA_VERSION;
    std::string              id;
    std::string              host;
    int                      port          = 30303;
    bool                     trusted       = false;
    int                      priority      = 2;
    std::string              region;
    std::vector<std::string> tags;
    int                      banCount      = 0;
    uint64_t                 bannedUntil   = 0;
    double                   score         = 1.0;
    uint64_t                 lastSeen      = 0;
    uint64_t                 bytesReceived = 0;
    uint64_t                 bytesSent     = 0;
    uint64_t                 blocksRelayed = 0;
};

// =============================================================================
// PEER STORE
//
// Persists peer state, bans and metrics to RocksDB.
// Fix 1: db_ is raw pointer closed in destructor -- never double-freed.
// Fix 2: all methods log RocksDB error strings on failure.
// Fix 3: all serialization uses nlohmann::json with explicit field names.
// Fix 4: shared_mutex -- concurrent reads never block each other.
// Fix 5: schema version embedded in every record.
// Fix 6: all deserialization validates fields before use.
// =============================================================================
class PeerStore {
public:
    explicit PeerStore(const std::string &dbPath);
    ~PeerStore();

    PeerStore(const PeerStore&)            = delete;
    PeerStore& operator=(const PeerStore&) = delete;

    bool isOpen() const noexcept;

    // =========================================================================
    // PEER OPERATIONS
    // =========================================================================
    bool savePeer  (const PersistedPeer &peer)         noexcept;
    bool loadPeer  (const std::string   &id,
                     PersistedPeer       &out)          noexcept;
    bool deletePeer(const std::string   &id)            noexcept;
    std::vector<PersistedPeer> loadAllPeers()           noexcept;

    // =========================================================================
    // BAN OPERATIONS
    // =========================================================================
    bool saveBan (const std::string &id,
                   uint64_t           bannedUntil,
                   int                banCount)         noexcept;
    bool loadBan (const std::string  &id,
                   uint64_t           &bannedUntil,
                   int                &banCount)        noexcept;
    bool clearBan(const std::string  &id)               noexcept;
    std::unordered_map<std::string,
                       uint64_t> loadAllBans()          noexcept;

    // =========================================================================
    // METRICS OPERATIONS
    // =========================================================================
    bool saveMetrics(const nlohmann::json &metrics)     noexcept;
    bool loadMetrics(nlohmann::json       &out)         noexcept;

private:
    // Fix 1: raw pointer -- opened once, deleted once in destructor
    rocksdb::DB*      db_   = nullptr;
    rocksdb::Options  opts_;

    // Fix 4: shared_mutex allows concurrent reads
    mutable std::shared_mutex mu_;

    static constexpr const char* PREFIX_PEER = "peer:";
    static constexpr const char* PREFIX_BAN  = "ban:";
    static constexpr const char* KEY_METRICS = "__net_metrics__";

    // Fix 2: logs RocksDB error strings
    void log(int level, const std::string &msg) const noexcept;

    // Fix 3: serialization helpers
    static nlohmann::json  peerToJson(const PersistedPeer &peer) noexcept;
    static bool            jsonToPeer(const nlohmann::json &j,
                                       PersistedPeer        &out) noexcept;
    static nlohmann::json  banToJson (uint64_t bannedUntil,
                                       int      banCount)          noexcept;
};
