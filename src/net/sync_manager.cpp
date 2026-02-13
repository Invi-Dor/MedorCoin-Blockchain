#include "net/sync_manager.h"
#include "net/serialization.h"
#include "net/net_manager.h"
#include <iostream>

SyncManager::SyncManager(Blockchain &chainRef)
    : chain(chainRef) {}

void SyncManager::handleSyncBlock(const nlohmann::json &msg) {
    if (msg["type"] != "sync_block") return;

    // Deserialize
    Block blk = deserializeBlock(msg["block"]);

    // Basic check: previousHash must match last block
    if (chain.chain.empty() ||
        blk.previousHash == chain.chain.back().hash) {

        chain.chain.push_back(blk);
        std::cout << "[SYNC] Added synced block: " << blk.hash << std::endl;
    }
}

void SyncManager::handlePeerHeight(const std::string &peerId, uint64_t height) {
    uint64_t localHeight = chain.chain.size();

    if (height > localHeight) {
        // Ask the peer for missing blocks
        nlohmann::json req;
        req["type"] = "sync_request";
        req["fromIndex"] = localHeight;

        NetworkManager net(""); // pass your listen address
        net.broadcastBlock(req);

        std::cout << "[SYNC] Requested blocks from " << localHeight
                  << " from peer " << peerId << std::endl;
    }
}
