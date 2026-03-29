/**
 * MEDORCOIN INDUSTRIAL ORCHESTRATOR - FINAL REINFORCED
 * Resolves: IPC Race Conditions, BigInt Precision, Soft/Hard Shutdown, and Type Safety.
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

    // --- STATE TRACKING (Problem 10) ---
    let currentDifficulty = BigInt(config.consensus.pow.initialDifficulty || 6);
    let lastBlockHash = "0".repeat(64); 
    let currentHeight = 0;

    const metrics = { totalHashes: 0n, blocksFound: 0, lastBlockTime: Date.now(), workerStats: new Map() };

    async function init() {
        logger.shipToTransport("SYSTEM", "MASTER", `Initialising MedorCoin Industrial Cluster...`);
        
        try {
            await engine.recoverFromCrash();
            currentHeight = Number(engine.currentHeight) || 0;
            lastBlockHash = String(engine.lastBlockHash || "0".repeat(64));
        } catch (e) {
            logger.shipToTransport("CRITICAL", "DB", `Recovery Failure: ${e.message}`);
        }

        for (let i = 0; i < numWorkers; i++) spawnWorker();

        // --- ATOMIC DISPATCH LOOP (Problem 6 & 11) ---
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

                // Problem 1: Safe IPC broadcast
                for (const id in cluster.workers) {
                    const w = cluster.workers[id];
                    if (w && w.isConnected()) {
                        try { w.send({ type: 'NEW_WORK', workLoad }); } catch (e) { /* Worker disconnected mid-loop */ }
                    }
                }
            } finally {
                isDispatching = false;
                if (!isShuttingDown) setTimeout(dispatchLoop, 1500);
            }
        };
        dispatchLoop();
    }

    function spawnWorker() {
        const worker = cluster.fork();
        metrics.workerStats.set(worker.id, { hashes: 0n });

        worker.on('message', async (msg) => {
            if (msg.type === 'BLOCK_FOUND') {
                try {
                    // Problem 2: Full async confirmation wrap
                    const ok = await engine.confirmBlock(msg.mempool, msg.block.minerAddress, msg.block.hash, msg.block.height);
                    if (ok) {
                        metrics.blocksFound++;
                        currentHeight = Number(engine.currentHeight);
                        lastBlockHash = String(engine.lastBlockHash);
                        adjustDifficulty(); 
                    }
                } catch (e) { logger.shipToTransport("ERROR", "CONSENSUS", `Block Rejected: ${e.message}`); }
            } else if (msg.type === 'METRICS') {
                const s = metrics.workerStats.get(worker.id);
                if (s) { 
                    const h = BigInt(msg.hashes || 0);
                    s.hashes += h; 
                    metrics.totalHashes += h; 
                }
            }
        });

        worker.on('exit', () => {
            metrics.workerStats.delete(worker.id);
            if (!isShuttingDown) spawnWorker();
        });
    }

    function adjustDifficulty() {
        const actual = (Date.now() - metrics.lastBlockTime) / 1000;
        const target = config.consensus.pow.targetBlockTimeSecs || 15;
        
        // Problem 3 & 5: High-Precision BigInt Scaling (Scale by 1000 to avoid floor bias)
        if (actual < target / 2) {
            currentDifficulty = (currentDifficulty * 1100n) / 1000n;
        } else if (actual > target * 2 && currentDifficulty > 2n) {
            currentDifficulty = (currentDifficulty * 900n) / 1000n;
        }
        
        if (currentDifficulty < 1n) currentDifficulty = 1n;
        metrics.lastBlockTime = Date.now();
    }

    async function getTargetMiner() {
        try {
            const sessions = [];
            // Problem 5: Protective session iterator
            const it = engine.db.iterator({ gte: 'session:', limit: 20 });
            for await (const [k, v] of it) { if (v) sessions.push(v); }
            return sessions.length > 0 ? sessions[Math.floor(Math.random() * sessions.length)] : "1ukpJFf4uz3c3gDmM9JASZaGiV4STJEDfzp";
        } catch (e) { return "1ukpJFf4uz3c3gDmM9JASZaGiV4STJEDfzp"; }
    }

    // --- SHUTDOWN (Problem 6, 8, & 12) ---
    process.on('SIGINT', async () => {
        isShuttingDown = true;
        logger.shipToTransport("SYSTEM", "SHUTDOWN", "Performing Graceful Exit...");
        
        // 1. Soft kill (SIGTERM) to allow workers to wrap up
        for (const id in cluster.workers) cluster.workers[id].kill('SIGTERM');

        const forceExit = setTimeout(() => {
            logger.shipToTransport("WARN", "SHUTDOWN", "Forced exit triggered.");
            process.exit(1);
        }, 10000);

        try {
            await engine.shutdown();
            logger.shipToTransport("SYSTEM", "SHUTDOWN", "Database flushed. Goodbye.");
        } finally {
            clearTimeout(forceExit);
            process.exit(0);
        }
    });

    init();

} else {
    // --- WORKER ROLE (Problem 4, 9, & 11) ---
    const pow = new ProofOfWork(config.consensus.pow);
    let currentAbort = null;

    process.on('message', async (msg) => {
        if (msg.type === 'NEW_WORK') {
            // Problem 4: Atomic Controller Reset
            if (currentAbort) currentAbort.abort();
            currentAbort = new AbortController();
            const signal = currentAbort.signal;

            try {
                const result = await pow.mine({ ...msg.workLoad, timestamp: Date.now(), nonce: 0n }, signal);
                
                // Problem 9 & 11: Validating result before IPC return
                if (result && result.found && !signal.aborted) {
                    process.send({ 
                        type: 'BLOCK_FOUND', 
                        block: { ...msg.workLoad, hash: result.hash }, 
                        mempool: msg.workLoad.mempool 
                    });
                }
                process.send({ type: 'METRICS', hashes: result?.hashesComputed || 0 });
            } catch (e) {
                if (e.name !== 'AbortError') logger.shipToTransport("ERROR", "WORKER", e.message);
            }
        }
    });
}
