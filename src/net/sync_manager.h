#pragma once

#include <string>
#include <vector>
#include "blockchain.h"
#include "block.h"
#include <nlohmann/json.hpp>

/**
 * SyncManager - sync blocks between peers
 */
class SyncManager {
public:
    explicit SyncManager(Blockchain &chain);

    // Called when a peer announces its latest height
    void handlePeerHeight(const std::string &peerId, uint64_t height);

    // Process a sync block message received from network
    void handleSyncBlockMsg(const nlohmann::json &msg);

    // Request missing blocks from peer
    void requestBlocks(const std::string &peerId, uint64_t from);

private:
    Blockchain &blockchain;
};
