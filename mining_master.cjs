/**
 * MEDORCOIN INDUSTRIAL MINING MASTER - ULTIMATE EDITION
 * Resolves: Block Spillover, Adaptive Buffering, and Reward Anomaly Detection.
 */

const cluster = require('cluster');
const os = require('os');
const TransactionEngine = require('./transaction_engine.cjs');
const ProofOfWork = require('./proof_of_work.cjs');
const MempoolManager = require('./mempool.cjs');
const config = require('./consensus_config.json');
const logger = require('./log_transport.cjs');

if (cluster.isMaster) {
    const engine = new TransactionEngine({ nodeId: "medor-master-industrial" });
    const mempool = new MempoolManager(engine.db);
    const numWorkers = config.consensus.pow.threads || os.cpus().length;
    
    let isShuttingDown = false;
    let isDispatching = false;
    
    // PROBLEM FIX 1: Two-Tier Submission Queue (Active + Spillover)
    const MAX_PRIMARY_QUEUE = 50; 
    const submissionQueue = []; 
    const spilloverBuffer = []; 

    let currentDifficulty = BigInt(config.consensus.pow.initialDifficulty || 6);
    let lastBlockHash = "0".repeat(64); 
    let currentHeight = 0;

    const metrics = { 
        totalHashes: 0n, 
        blocksFound: 0, 
        lastBlockConfirmedTime: Date.now(),
        workerStats: new Map(),
        fallbackMinerCount: 0,
        consecutiveFallbacks: 0 // PROBLEM FIX 4: Anomaly Detection
    };

    async function init() {
        logger.shipToTransport("SYSTEM", "MASTER", `Booting Industrial Cluster...`);
        
        try {
            await engine.recoverFromCrash();
            currentHeight = Number(engine.currentHeight) || 0;
            lastBlockHash = String(engine.lastBlockHash || "0".repeat(64));
            metrics.lastBlockConfirmedTime = Date.now(); 
        } catch (e) {
            logger.shipToTransport("CRITICAL", "DB", `Recovery Failure: ${e.message}`);
        }

        for (let i = 0; i < numWorkers; i++) spawnWorker();

        const dispatchLoop = async () => {
            if (isShuttingDown || isDispatching) return;
            isDispatching = true;
            
            try {
                const txs = await mempool.getPrioritized(config.consensus.validation.maxTransactionsPerBlock || 1000).catch(() => []);
                const targetMiner = await getTargetMiner();
                
                const workLoad = {
                    previousHash: String(lastBlockHash),
                    difficulty: String(currentDifficulty),
                    minerAddress: String(targetMiner),
                    height: currentHeight + 1,
                    mempool: txs
                };

                for (const id in cluster.workers) {
                    const w = cluster.workers[id];
                    if (w && w.isConnected()) {
                        try { w.send({ type: 'NEW_WORK', workLoad }); } catch (e) {}
                    }
                }
            } finally {
                isDispatching = false;
                if (!isShuttingDown) setTimeout(dispatchLoop, 1500);
            }
        };
        dispatchLoop();
        processQueue(); 
    }

    async function processQueue() {
        if (isShuttingDown) return;

        // PROBLEM FIX 1: Drain spillover into primary if space permits
        if (submissionQueue.length < MAX_PRIMARY_QUEUE && spilloverBuffer.length > 0) {
            submissionQueue.push(spilloverBuffer.shift());
        }

        if (submissionQueue.length > 0) {
            const { block, txs } = submissionQueue.shift();
            try {
                if (block.height > currentHeight) {
                    const ok = await engine.confirmBlock(txs, block.minerAddress, block.hash, block.height);
                    if (ok) {
                        metrics.blocksFound++;
                        const now = Date.now();
                        const blockInterval = (now - metrics.lastBlockConfirmedTime) / 1000;
                        adjustDifficulty(blockInterval);
                        
                        metrics.lastBlockConfirmedTime = now;
                        currentHeight = Number(engine.currentHeight);
                        lastBlockHash = String(engine.lastBlockHash);
                    }
                }
            } catch (e) {
                logger.shipToTransport("ERROR", "CONSENSUS", `Submission Failed: ${e.message}`);
            }
        }
        setImmediate(processQueue);
    }

    function spawnWorker() {
        const worker = cluster.fork();
        metrics.workerStats.set(worker.id, { hashes: 0n });

        worker.on('message', (msg) => {
            if (msg.type === 'BLOCK_FOUND') {
                // PROBLEM FIX 1: Spillover logic instead of a hard drop
                if (submissionQueue.length >= MAX_PRIMARY_QUEUE) {
                    if (spilloverBuffer.length < 100) {
                        spilloverBuffer.push({ block: msg.block, txs: msg.mempool });
                        logger.shipToTransport("WARN", "QUEUE", "Primary queue full. Block moved to spillover buffer.");
                    } else {
                        logger.shipToTransport("CRITICAL", "QUEUE", "All buffers exhausted. Dropping block.");
                    }
                } else {
                    submissionQueue.push({ block: msg.block, txs: msg.mempool });
                }
            } else if (msg.type === 'METRICS') {
                const s = metrics.workerStats.get(worker.id);
                if (s) { 
                    const h = BigInt(msg.hashes || 0);
                    s.hashes += h; 
                    metrics.totalHashes += h; 
                }
            } else if (msg.type === 'ERROR') {
                logger.shipToTransport("ERROR", `WORKER_${worker.id}`, msg.error);
            }
        });

        worker.on('exit', () => {
            metrics.workerStats.delete(worker.id);
            if (!isShuttingDown) spawnWorker();
        });
    }

    function adjustDifficulty(actualInterval) {
        const target = config.consensus.pow.targetBlockTimeSecs || 15;
        if (actualInterval < target / 2) {
            currentDifficulty = (currentDifficulty * 1100n) / 1000n;
        } else if (actualInterval > target * 2 && currentDifficulty > 2n) {
            currentDifficulty = (currentDifficulty * 900n) / 1000n;
        }
        if (currentDifficulty < 1n) currentDifficulty = 1n;
    }

    async function getTargetMiner() {
        try {
            const sessions = [];
            const it = engine.db.iterator({ gte: 'session:', limit: 20 });
            for await (const [k, v] of it) { if (v) sessions.push(v); }
            
            if (sessions.length > 0) {
                metrics.consecutiveFallbacks = 0; // Reset on success
                return sessions[Math.floor(Math.random() * sessions.length)];
            }
            
            // PROBLEM FIX 4: Consecutive Fallback Alerting
            metrics.fallbackMinerCount++;
            metrics.consecutiveFallbacks++;
            
            if (metrics.consecutiveFallbacks > 50) {
                logger.shipToTransport("ALERT", "REWARDS", `CRITICAL: ${metrics.consecutiveFallbacks} consecutive fallback rewards. Check Session Manager!`);
            } else if (metrics.fallbackMinerCount % 10 === 0) {
                logger.shipToTransport("INFO", "REWARDS", `Fallback miner used ${metrics.fallbackMinerCount} times.`);
            }
            return "1ukpJFf4uz3c3gDmM9JASZaGiV4STJEDfzp";
        } catch (e) { return "1ukpJFf4uz3c3gDmM9JASZaGiV4STJEDfzp"; }
    }

    process.on('SIGINT', async () => {
        isShuttingDown = true;
        logger.shipToTransport("SYSTEM", "SHUTDOWN", "Draining Workers...");
        
        const workerExits = Object.values(cluster.workers).map(w => {
            return new Promise(resolve => {
                w.on('exit', resolve);
                w.kill('SIGTERM');
            });
        });

        const forceExit = setTimeout(() => process.exit(1), 8000);

        try {
            await Promise.all(workerExits);
            await engine.shutdown();
        } finally {
            clearTimeout(forceExit);
            process.exit(0);
        }
    });

    init();

} else {
    // --- WORKER ROLE ---
    const pow = new ProofOfWork(config.consensus.pow);
    let currentAbort = null;
    
    // PROBLEM FIX 2: Dynamic work buffer (Queue size 1)
    const taskQueue = [];

    process.on('message', (msg) => {
        if (msg.type === 'NEW_WORK') {
            taskQueue.push(msg.workLoad);
            if (taskQueue.length > 1) taskQueue.shift(); 
            processNextTask();
        }
    });

    async function processNextTask() {
        if (taskQueue.length === 0) return;
        
        const workLoad = taskQueue.pop(); 
        taskQueue.length = 0; 

        if (currentAbort) currentAbort.abort();
        currentAbort = new AbortController();
        const signal = currentAbort.signal;

        try {
            const result = await pow.mine({ ...workLoad, timestamp: Date.now(), nonce: 0n }, signal);
            
            if (result && result.found && !signal.aborted) {
                process.send({ 
                    type: 'BLOCK_FOUND', 
                    block: { ...workLoad, hash: result.hash }, 
                    mempool: workLoad.mempool 
                });
            }
            process.send({ type: 'METRICS', hashes: result?.hashesComputed || 0 });
        } catch (e) {
            if (e.name !== 'AbortError') process.send({ type: 'ERROR', error: e.message });
        }
    }
}
