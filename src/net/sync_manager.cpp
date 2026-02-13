#include "net/sync_manager.h"
#include "net/serialization.h"
#include "net/net_manager.h"
#include <iostream>

SyncManager::SyncManager(Blockchain &chain)
    : blockchain(chain) {}

void SyncManager::handlePeerHeight(const std::string &peerId, uint64_t height) {
    uint64_t localHeight = blockchain.chain.size();

    // If peer has more blocks, request missing
    if (height > localHeight) {
        requestBlocks(peerId, localHeight);
    }
}

void SyncManager::requestBlocks(const std::string &peerId, uint64_t from) {
    nlohmann::json msg;
    msg["type"] = "sync_request";
    msg["from"] = from;

    // Use NetworkManager to broadcast or send specific peer
    NetworkManager net(""); // set actual listen address on init
    net.broadcastBlock(msg);
    std::cout << "[Sync] requested blocks from " << from << " from " << peerId << std::endl;
}

void SyncManager::handleSyncBlockMsg(const nlohmann::json &msg) {
    if (msg["type"] != "sync_block") return;

    // Deserialize block from message
    Block b = deserializeBlock(msg["block"]);

    // Accept only if previous hash matches
    if (blockchain.chain.empty() ||
        b.previousHash == blockchain.chain.back().hash) {
        blockchain.chain.push_back(b);
        std::cout << "[Sync] synced block " << b.hash << std::endl;
    }
}
