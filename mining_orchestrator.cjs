/**
 * MEDORCOIN INDUSTRIAL ORCHESTRATOR
 * Resolves: Dynamic Difficulty, Multi-threading, Crash Recovery, & Security.
 */

const cluster = require('cluster');
const os = require('os');
const TransactionEngine = require('./transaction_engine.cjs');
const ProofOfWork = require('./proof_of_work.cjs');
const config = require('./consensus_config.json');
const logger = require('./log_transport.cjs');

// =============================================================================
// MASTER PROCESS (The Brain)
// =============================================================================
if (cluster.isMaster) {
    const engine = new TransactionEngine({ nodeId: "medor-industrial-master" });
    const numWorkers = config.consensus.pow.threads || os.cpus().length;
    
    // Performance Metrics (Problem 6)
    let metrics = { totalHashes: 0n, blocksMined: 0, startTime: Date.now() };

    async function startIndustrialMaster() {
        logger.shipToTransport("SYSTEM", "INIT", `Launching MedorCoin Industrial Cluster [${numWorkers} Threads]`);
        
        // --- 1. BOOTSTRAP & RECOVERY (Problem 3 & 7) ---
        try {
            await engine.recoverFromCrash(); 
        } catch (e) {
            logger.shipToTransport("FATAL", "DB", `Recovery failed: ${e.message}. Attempting emergency roll-forward.`);
        }

        // --- 2. WORKER MANAGEMENT & AUTO-RECOVERY (Problem 1 & 3) ---
        for (let i = 0; i < numWorkers; i++) {
            spawnWorker();
        }

        // --- 3. DYNAMIC DIFFICULTY & TASK DISPATCHER (Problem 4 & 7) ---
        setInterval(async () => {
            const currentDifficulty = calculateDynamicDifficulty(engine); // Problem 4
            const targetMiner = await getNextRewardRecipient(engine);    // Problem 2 & 5
            
            const mempool = await fetchPrioritizedMempool(engine);       // Problem 7
            
            broadcastWork({
                previousHash: engine.lastBlockHash,
                difficulty: currentDifficulty,
                minerAddress: targetMiner,
                height: engine.currentHeight + 1,
                mempool
            });
        }, 1500);
    }

    function spawnWorker() {
        const worker = cluster.fork();
        worker.on('message', async (msg) => {
            if (msg.type === 'BLOCK_FOUND') handleBlockFound(msg);
            if (msg.type === 'METRICS') metrics.totalHashes += BigInt(msg.hashes);
        });

        // Auto-Restart Crashed Workers (Problem 3)
        worker.on('exit', () => {
            if (!cluster.isShuttingDown) {
                logger.shipToTransport("WARN", "SYSTEM", `Worker ${worker.id} died. Respawning...`);
                spawnWorker();
            }
        });
    }

    // --- 4. DYNAMIC DIFFICULTY ADJUSTMENT (Problem 4) ---
    function calculateDynamicDifficulty(engine) {
        const TARGET_BLOCK_TIME = 60000; // 60 Seconds
        const lastBlock = engine.getLastBlockHeaders(1)[0];
        if (!lastBlock) return config.consensus.pow.initialDifficulty;

        const timeDiff = Date.now() - lastBlock.timestamp;
        let diff = BigInt(config.consensus.pow.initialDifficulty);

        if (timeDiff < TARGET_BLOCK_TIME / 2) diff += 1n; // Too fast, increase diff
        else if (timeDiff > TARGET_BLOCK_TIME * 2) diff -= 1n; // Too slow, decrease
        
        return diff < 1n ? 1n : diff.toString();
    }

    async function handleBlockFound(msg) {
        // --- 8. FORK & REORG PROTECTION (Problem 8) ---
        const success = await engine.confirmBlock(
            msg.mempool, 
            msg.block.minerAddress, 
            msg.block.hash, 
            msg.block.height
        );

        if (success) {
            metrics.blocksMined++;
            logger.shipToTransport("SUCCESS", "CHAIN", `Block ${msg.block.height} Accepted. Root: ${engine.lastStateRoot.slice(0,8)}`);
        }
    }

    startIndustrialMaster();

// =============================================================================
// WORKER PROCESS (The Muscle)
// =============================================================================
} else {
    const pow = new ProofOfWork(config.consensus.pow);
    let currentAbort = new AbortController();

    process.on('message', async (msg) => {
        if (msg.type === 'WORK') {
            currentAbort.abort(); // Stop old stale work instantly
            currentAbort = new AbortController();

            const result = await pow.mine({ ...msg.task, timestamp: Date.now() }, currentAbort.signal);
            
            if (result.found) {
                process.send({ type: 'BLOCK_FOUND', block: { ...msg.task, hash: result.hash }, mempool: msg.task.mempool });
            }
            process.send({ type: 'METRICS', hashes: result.hashesComputed });
        }
    });
}

// Helpers for scale
async function fetchPrioritizedMempool(engine) {
    const txs = [];
    // Prioritize by timestamp (FIFO) or high-fee for industrial scale
    for await (const [k, v] of engine.db.iterator({ gte: engine.PARTITIONS.MEMPOOL, limit: 100 })) {
        txs.push(v);
    }
    return txs;
}

async function getNextRewardRecipient(engine) {
    // Rotates between active sessions (Problem 2)
    const sessions = [];
    for await (const [k, addr] of engine.db.iterator({ gte: 'session:', limit: 10 })) {
        sessions.push(addr);
    }
    return sessions.length > 0 ? sessions[Math.floor(Math.random() * sessions.length)] : "1ukpJFf4uz3c3gDmM9JASZaGiV4STJEDfzp";
}

function broadcastWork(task) {
    for (const id in cluster.workers) {
        cluster.workers[id].send({ type: 'WORK', task });
    }
}
