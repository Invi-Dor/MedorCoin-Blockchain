/**
 * MEDORCOIN INDUSTRIAL MINING WORKER
 * Version: 2.0 (Production Grade)
 * Resolves: Multi-threading, Dynamic Rewards, and Non-Blocking Mempool.
 */

const cluster = require('cluster');
const os = require('os');
const TransactionEngine = require('./transaction_engine.cjs');
const ProofOfWork = require('./proof_of_work.cjs');
const config = require('./consensus_config.json');
const logger = require('./log_transport.cjs');

// =============================================================================
// ROLE 1: THE MASTER (Database & Orchestration)
// =============================================================================
if (cluster.isMaster) {
    const engine = new TransactionEngine({ nodeId: "medor-industrial-node" });
    const numWorkers = config.consensus.pow.threads || os.cpus().length;

    async function startMaster() {
        logger.shipToTransport("SYSTEM", "MASTER", `Scaling to ${numWorkers} CPU Cores...`);
        await engine.recoverFromCrash();

        // Fork Workers
        for (let i = 0; i < numWorkers; i++) {
            const worker = cluster.fork();
            
            // Handle block submission from any worker
            worker.on('message', async (msg) => {
                if (msg.type === 'BLOCK_FOUND') {
                    await handleBlockSubmission(msg.block, msg.txs);
                }
            });
        }

        // --- PROBLEM FIX 2: NON-BLOCKING TASK DISTRIBUTION ---
        // We push work to workers every second without stopping their hashing
        setInterval(async () => {
            const mempoolTxs = [];
            for await (const [k, v] of engine.db.iterator({ gte: engine.PARTITIONS.MEMPOOL, limit: config.consensus.validation.maxTransactionsPerBlock })) {
                mempoolTxs.push(v);
            }

            // PROBLEM FIX 3: DYNAMIC USER REWARD
            // Get the latest active session from DB (Logic to be expanded per auth_service)
            const activeUser = await engine.db.get('active_session_addr').catch(() => "1ukpJFf4uz3c3gDmM9JASZaGiV4STJEDfzp");

            const workLoad = {
                previousHash: engine.lastBlockHash,
                difficulty: config.consensus.pow.initialDifficulty,
                minerAddress: activeUser,
                height: engine.currentHeight + 1,
                txs: mempoolTxs
            };

            // Broadcast to all workers
            for (const id in cluster.workers) {
                cluster.workers[id].send({ type: 'NEW_WORK', workLoad });
            }
        }, 1500);
    }

    async function handleBlockSubmission(block, txs) {
        try {
            // PROBLEM FIX 4: ATOMIC SUBMISSION
            const success = await engine.confirmBlock(txs, block.minerAddress, block.hash, block.height);
            if (success) {
                logger.shipToTransport("SUCCESS", "MINER", `Block ${block.height} Confirmed by ${block.minerAddress}`);
                
                // Snapshot Logic
                if (block.height % config.consensus.storage.snapshotInterval === 0) {
                    await engine.createSnapshot(block.height - 999, block.height, true);
                }
            }
        } catch (e) {
            logger.shipToTransport("ERROR", "MASTER", `Submission Rejected: ${e.message}`);
        }
    }

    startMaster();

// =============================================================================
// ROLE 2: THE WORKER (Pure Mathematical Hashing)
// =============================================================================
} else {
    const pow = new ProofOfWork(config.consensus.pow);
    let abortController = new AbortController();

    process.on('message', async (msg) => {
        if (msg.type === 'NEW_WORK') {
            // PROBLEM FIX 4: ABORT STALE WORK
            // Stop hashing old block height immediately
            abortController.abort();
            abortController = new AbortController();

            const { workLoad } = msg;
            const blockTemplate = {
                previousHash: workLoad.previousHash,
                difficulty: workLoad.difficulty,
                minerAddress: workLoad.minerAddress,
                timestamp: Date.now(),
                nonce: 0n
            };

            // Run PoW Hashing
            const result = await pow.mine(blockTemplate, abortController.signal);
            
            if (result.found) {
                process.send({ 
                    type: 'BLOCK_FOUND', 
                    block: { ...blockTemplate, hash: result.hash, height: workLoad.height },
                    txs: workLoad.txs 
                });
            }
        }
    });
}
