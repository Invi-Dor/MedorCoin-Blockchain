#include "main.h"
#include "blockchain.h"
#include "transaction.h"
#include "net/net_manager.h"
#include "net/sync_manager.h"
#include "crypto/keystore.h"
#include "utxo.h"
#include <iostream>
#include <vector>
#include <string>
#include <csignal>

// Global pointers
std::unique_ptr<NetworkManager> netMgr;
std::unique_ptr<SyncManager> syncMgr;
std::unique_ptr<Blockchain> chainPtr;

// Graceful shutdown
static bool running = true;
void handleSignal(int) {
    running = false;
}

int main(int argc, char* argv[]) {
    // Handle shutdown signals
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // 1) Load config
    std::string listenAddr = "0.0.0.0:4001";
    if (argc > 1) listenAddr = argv[1];

    std::cout << "[NODE] Starting MedorCoin Node at " << listenAddr << std::endl;

    // 2) Initialize blockchain
    chainPtr = std::make_unique<Blockchain>(/*ownerAddress=*/"medor_owner");
    Blockchain &medorChain = *chainPtr;

    // 3) Initialize networking
    netMgr = std::make_unique<NetworkManager>(listenAddr);
    NetworkManager &network = *netMgr;

    if (!network.start()) {
        std::cerr << "[NETWORK] Failed to start network manager" << std::endl;
        return -1;
    }

    // 4) Sync manager to handle chain sync
    syncMgr = std::make_unique<SyncManager>(medorChain);
    SyncManager &syncManager = *syncMgr;

    // 5) Register message handler
    network.onMessage([&](const nlohmann::json &msg) {
        std::string type = msg.value("type", "");

        if (type == "sync_request") {
            uint64_t fromIndex = msg["fromIndex"].get<uint64_t>();
            for (size_t i = fromIndex; i < medorChain.chain.size(); ++i) {
                nlohmann::json blkMsg;
                blkMsg["type"] = "sync_block";
                blkMsg["block"] = serializeBlock(medorChain.chain[i]);
                network.broadcastBlock(blkMsg);
            }
        }
        else if (type == "sync_block") {
            syncManager.handleSyncBlock(msg);
        }
        else if (type == "announce_height") {
            std::string peer = msg.value("peerId", "");
            uint64_t height = msg.value("height", uint64_t(0));
            syncManager.handlePeerHeight(peer, height);
        }
        else if (type == "tx_broadcast") {
            Transaction received = deserializeTx(msg["tx"]);
            medorChain.mempool.addTransaction(received);
        }
        else if (type == "block_broadcast") {
            Block received = deserializeBlock(msg["block"]);
            // Optional: full validation before adding
            medorChain.chain.push_back(received);
        }
    });

    // 6) Announce own height to peers
    auto announceHeight = [&]() {
        nlohmann::json heightMsg;
        heightMsg["type"] = "announce_height";
        heightMsg["peerId"] = listenAddr;
        heightMsg["height"] = medorChain.chain.size();
        network.broadcastBlock(heightMsg);
    };

    announceHeight();

    std::cout << "[NODE] Initialization complete — entering main loop" << std::endl;

    // 7) Main loop
    while (running) {
        announceHeight();
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }

    std::cout << "[NODE] Shutting down" << std::endl;
    return 0;
}
