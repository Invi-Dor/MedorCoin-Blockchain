#pragma once

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "blockchain.h"

/**
 * SyncManager handles block synchronization messages
 * and triggers longest‑chain fork resolution.
 */
class SyncManager {
public:
    explicit SyncManager(Blockchain &chainRef);

    /**
     * Called when a "sync_block" message arrives.
     */
    void handleSyncBlock(const nlohmann::json &msg);

    /**
     * Called when a peer announces its height.
     */
    void handlePeerHeight(const std::string &peerId, uint64_t height);

private:
    Blockchain &chain;
};
