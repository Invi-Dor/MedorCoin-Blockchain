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
#include <thread>

// Globals
std::unique_ptr<NetworkManager> netMgr;
std::unique_ptr<SyncManager> syncMgr;
std::unique_ptr<Blockchain> chainPtr;
bool running = true;

// Shutdown handler
void handleSignal(int) {
    running = false;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::string listenAddr = "0.0.0.0:4001";
    if (argc > 1) listenAddr = argv[1];

    std::cout << "[NODE] Starting MedorCoin Node with address: " << listenAddr << std::endl;

    chainPtr = std::make_unique<Blockchain>("medor_owner");
    Blockchain &medorChain = *chainPtr;

    netMgr = std::make_unique<NetworkManager>(listenAddr);
    NetworkManager &network = *netMgr;

    if (!network.start()) {
        std::cerr << "[NETWORK] Failed to start network manager" << std::endl;
        return -1;
    }

    syncMgr = std::make_unique<SyncManager>(medorChain);
    SyncManager &syncManager = *syncMgr;

    network.onMessage([&](const nlohmann::json &msg) {
        std::string type = msg.value("type", "");

        if (type == "sync_request") {
            uint64_t fromIndex = msg.value("fromIndex", uint64_t(0));
            for (size_t i = fromIndex; i < medorChain.chain.size(); ++i) {
                nlohmann::json blkMsg;
                blkMsg["type"] = "sync_block";
                blkMsg["block"] = serializeBlock(medorChain.chain[i]);
                network.broadcastBlock(blkMsg);
            }

        } else if (type == "sync_block") {
            syncManager.handleSyncBlock(msg);

        } else if (type == "announce_height") {
            std::string peer = msg.value("peerId", "");
            uint64_t height = msg.value("height", uint64_t(0));
            syncManager.handlePeerHeight(peer, height);

        } else if (type == "tx_broadcast") {
            Transaction received = deserializeTx(msg["tx"]);
            medorChain.mempool.addTransaction(received);

        } else if (type == "block_broadcast") {
            Block received = deserializeBlock(msg["block"]);
            medorChain.chain.push_back(received);
        }
    });

    auto announceHeight = [&]() {
        nlohmann::json h;
        h["type"] = "announce_height";
        h["peerId"] = listenAddr;
        h["height"] = medorChain.chain.size();
        network.broadcastBlock(h);
    };

    announceHeight();

    std::cout << "[NODE] Initialization complete, entering loop." << std::endl;

    while (running) {
        announceHeight();
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }

    std::cout << "[NODE] Node shutting down..." << std::endl;
    return 0;
}
