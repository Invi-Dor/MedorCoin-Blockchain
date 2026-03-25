/**
 * MedorCoin - Node Entry Point
 * Wires all components together, starts the node,
 * and ensures graceful shutdown on errors/signals.
 */

const logger = require("./logger");
const metrics = require("./metrics");
const scheduler = require("./scheduler");
const Storage = require("./storage");
const UTXOSet = require("./utxo_set");
const ConsensusEngine = require("./consensus");
const Mempool = require("./mempool");
const Miner = require("./mining");
const P2PNetwork = require("./p2p_network");
const RPCServer = require("./rpc_server");
const ValidatorRegistry = require("./validator_registry");

// ─── Configuration ───────────────────────────────────────────────
const CONFIG = {
    p2pPort: parseInt(process.env.P2P_PORT || "8333"),
    rpcPort: parseInt(process.env.RPC_PORT || "8332"),
    dataDir: process.env.DATA_DIR || "./data",
    mineEnabled: process.env.MINE === "true",
    seedPeers: (process.env.SEED_PEERS || "")
        .split(",")
        .filter(Boolean)
        .map((s) => {
            const [address, port] = s.split(":");
            return { address, port: parseInt(port || "8333") };
        }),
    wallet: { 
        address: process.env.WALLET_ADDRESS || "MDR_GENESIS_ADDRESS",
        privateKey: process.env.WALLET_PRIVKEY // Required for PoA Signing
    },
};

// ─── Bootstrap ───────────────────────────────────────────────────
async function main() {
    logger.info("NODE", "node.js", "=== MedorCoin Node Starting ===");
    logger.info("NODE", "node.js", `Config: P2P=${CONFIG.p2pPort}, RPC=${CONFIG.rpcPort}, Mine=${CONFIG.mineEnabled}`);

    try {
        // 1. Storage Layer (Base Dependency)
        const storage = new Storage(CONFIG.dataDir);
        await storage.init();

        // 2. State & Validator Layers
        const utxoSet = new UTXOSet(storage);
        // ValidatorRegistry must be initialized to know PoA turns
        const validatorRegistry = new ValidatorRegistry(storage);
        await validatorRegistry.init();

        // 3. Consensus Engine (Fixed constructor to match consensus.js)
        const chain = new ConsensusEngine(utxoSet, validatorRegistry, storage);
        // Chain depends on storage tip to know where we are
        const currentTip = await storage.getChainTip();
        logger.info("NODE", "node.js", `Current Chain Height: ${currentTip.height}`);

        // 4. Mempool (Depends on UTXO for validation)
        const mempool = new Mempool(utxoSet);

        // 5. P2P Network
        const p2p = new P2PNetwork(CONFIG.p2pPort, CONFIG.seedPeers);
        await p2p.start();

        // ─── Event Wiring (The Central Nervous System) ──────────────

        // A. Handle Incoming Network Data
        p2p.on("message", async (msg, peerId) => {
            try {
                if (msg.type === "tx") {
                    const ok = await mempool.addTransaction(msg.data);
                    if (ok) p2p.broadcast("tx", msg.data, peerId); // Relay valid txs
                } 
                else if (msg.type === "block") {
                    const result = await chain.addBlock(msg.data);
                    if (result.ok && result.status === "MAIN_CHAIN") {
                        // Relay blocks that extend our main chain
                        p2p.broadcast("block", msg.data, peerId);
                    } else if (!result.ok) {
                        // Fix: Penalize peers for sending invalid blocks (Security)
                        p2p.penalizePeer(peerId, 20, result.error);
                    }
                }
            } catch (err) {
                logger.error("NODE", "node.js", `P2P Logic Error: ${err.message}`);
            }
        });

        // B. Handle Local Chain Events
        chain.on("block_accepted", (block) => {
            logger.info("NODE", "node.js", `Block Accepted: ${block.height} (${block.hash})`);
            mempool.removeConfirmed(block.transactions); // Cleanup mempool
            metrics.gauge("medorcoin_chain_height", block.height);
        });

        chain.on("reorg_complete", (newHash) => {
            logger.warn("NODE", "node.js", `Chain Reorganized to: ${newHash}`);
        });

        // 6. RPC Server
        const rpc = new RPCServer(chain, mempool, p2p, CONFIG.wallet);
        rpc.start(CONFIG.rpcPort);

        // 7. Miner (Refactored to match hardened mining.js)
        let miner = null;
        if (CONFIG.mineEnabled) {
            if (!CONFIG.wallet.privateKey) {
                logger.error("MINER", "node.js", "Cannot start miner: Missing private key for PoA signing.");
            } else {
                miner = new Miner(mempool, chain, CONFIG.wallet);
                miner.start();
                rpc.setMiner(miner); // Allow RPC to control miner
                logger.info("NODE", "node.js", "Miner initialized and started.");
            }
        }

        // 8. Metrics & Monitoring
        metrics.startServer();

        // 9. Scheduled Maintenance Tasks
        scheduler.register("health_check", async () => {
            metrics.gauge("medorcoin_peer_count", p2p.peers.size);
            metrics.gauge("medorcoin_mempool_size", mempool.txs.size);
        }, 10000);

        scheduler.register("peer_discovery", async () => {
            if (p2p.peers.size < 3) {
                logger.info("P2P", "node.js", "Low peer count, seeking new connections...");
                p2p.discoverPeers();
            }
        }, 30000);

        // ─── Shutdown Logic ──────────────────────────────────────────
        scheduler.onShutdown(async () => {
            logger.info("NODE", "node.js", "Shutting down MedorCoin Node...");
            if (miner) miner.stop();
            rpc.stop();
            p2p.stop();
            await storage.close();
            logger.info("NODE", "node.js", "Shutdown complete.");
        });

    } catch (bootstrapError) {
        logger.error("NODE", "node.js", `Fatal Bootstrap Error: ${bootstrapError.message}`);
        process.exit(1);
    }
}

// ─── Global Guards ──────────────────────────────────────────────
const shutdown = async (signal) => {
    logger.info("NODE", "node.js", `Received ${signal}. Starting graceful exit.`);
    await scheduler.shutdown();
    process.exit(0);
};

process.on("SIGINT", () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));

process.on("uncaughtException", (err) => {
    logger.error("CRITICAL", "node.js", `Uncaught Exception: ${err.message}\n${err.stack}`);
});

process.on("unhandledRejection", (reason) => {
    logger.error("CRITICAL", "node.js", `Unhandled Rejection: ${reason}`);
});

main();
