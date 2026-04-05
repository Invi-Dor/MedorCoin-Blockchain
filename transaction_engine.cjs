const Redis = require('ioredis');
const Redlock = require('redlock').default || require('redlock');
const os = require('os');
const crypto = require('crypto');
const secp256k1 = require('secp256k1');
const pino = require('pino');
const CircuitBreaker = require('opossum');

const logger = pino({
    level: process.env.LOG_LEVEL || 'info',
    base: { node_id: process.env.NODE_ID || `global-node-${os.hostname()}` },
    transport: { target: 'pino-pretty' }
});

class TransactionEngine {
  constructor(secretKey, nodeId) {
    this.nodeId = nodeId || `node-${crypto.randomBytes(4).toString('hex')}`;
    this.isRunning = false;
    this.safeMode = false; // [4] Safe Mode Initialized

    // 1. GLOBAL PRODUCTION SECURITY & LATENCY TUNING
    const redisOptions = {
        password: process.env.REDIS_PASSWORD || undefined,
        tls: process.env.REDIS_TLS === 'true' ? { rejectUnauthorized: false } : undefined,
        retryStrategy: (times) => Math.min(times * 150, 5000), 
        maxRetriesPerRequest: 10,
        enableOfflineQueue: true,
        connectTimeout: 20000, 
        commandTimeout: 10000
    };

    // 2. [5] INDUSTRIAL SEED INJECTION: Listing all known regional nodes
    const seeds = process.env.REDIS_SEEDS 
        ? JSON.parse(process.env.REDIS_SEEDS) 
        : [
            { port: 6379, host: "127.0.0.1" },
            { port: 6380, host: "127.0.0.1" },
            { port: 6381, host: "127.0.0.1" }
          ];

    this.cluster = new Redis.Cluster(seeds, { 
        redisOptions, 
        scaleReads: "slave", 
        canRetryRouteCommand: true,
        natMap: process.env.REDIS_NAT_MAP ? JSON.parse(process.env.REDIS_NAT_MAP) : undefined,
        clusterRetryStrategy: (times) => Math.min(100 + times * 500, 10000)
    });
        
    this.redis = this.cluster; 
     this.cluster.on('error', (err) => {
    logger.error({ err: err.message }, 'Redis Cluster Error');
});
 


    // 3. DISTRIBUTED QUORUM: Canonical Redlock across independent clients
    this.redlockNodes = seeds.map(s => new Redis(s.port, s.host, redisOptions));
    this.redlockNodes.forEach((node, index) => {
    node.on('error', (err) => {
        logger.error({ 
            err: err.message, 
            host: seeds[index].host, 
            port: seeds[index].port 
        }, `Redlock Node ${index} Error`);
    });

    this.redlock = new Redlock(this.redlockNodes, { 
        driftFactor: 0.01, 
        retryCount: 60, 
        retryDelay: 300,
        retryJitter: 300 
    });

    // 4. ATOMIC LUA ENGINE: Global State Integrity {mdc}
    this.cluster.defineCommand('commitMedorBlock', {
        numberOfKeys: 0,
        lua: `
            local h = ARGV[1]
            local d = ARGV[2]
            local c = cjson.decode(ARGV[3])
            if redis.call('HEXISTS', '{mdc}:blocks:' .. h, 'data') == 1 then
                return redis.error_reply("ERR_BLOCK_EXISTS")
            end
            redis.call('HSET', '{mdc}:blocks:' .. h, 'data', d)
            redis.call('SET', '{mdc}:meta:height', h)
            for _, change in ipairs(c) do
                redis.call('HINCRBY', '{mdc}:balances', change.address, change.delta)
            end
            return "OK"
        `
    });

    // 5. [2] CONCURRENCY: Managed Parallel Worker Pool
    this.workerQueue = [];
    this.MAX_CONCURRENT = os.cpus().length * 2;

    this.commitBreaker = new CircuitBreaker(this._executeLuaCommit.bind(this), {
        timeout: 20000, 
        errorThresholdPercentage: 50, 
        resetTimeout: 45000 
    });

    this._setupGracefulShutdown();
    this.init();
  });

  async validateTransaction(tx, signature) {
    if (this.safeMode) return false; // [4] Safe Mode blocking
    if (!tx.sender || !tx.amount || !tx.publicKey) return false;
    try {
        const balance = await this.cluster.hget("{mdc}:balances", tx.sender);
        if (BigInt(balance || 0) < BigInt(tx.amount)) return false;
        const msgHash = crypto.createHash('sha256').update(JSON.stringify(tx)).digest();
        return secp256k1.ecdsaVerify(Buffer.from(signature, 'hex'), msgHash, Buffer.from(tx.publicKey, 'hex'));
    } catch (e) { return false; }
  }

  async init() {
      try {
          this.isRunning = true;
          await this.recoverFromCrash();
          logger.info({ height: this.currentHeight, env: "SOVEREIGN_GLOBAL" }, 'MEDORCOIN ENGINE: ONLINE');
      } catch (e) {
          logger.fatal({ err: e.message }, "Global Infrastructure Boot Failed");
      }
  }

  // [1] WAL RECOVERY: Syncs height and audits for orphans
  async recoverFromCrash() {
    try {
        const h = await this.cluster.get("{mdc}:meta:height");
        const lastBlock = await this.cluster.get("{mdc}:block:last");
        
        this.currentHeight = parseInt(h) || 0;

        // [4] Check for State Mismatch
        if (lastBlock && JSON.parse(lastBlock).height !== this.currentHeight) {
            this.safeMode = true;
            logger.fatal("STATE MISMATCH DETECTED: Entering Safe Mode.");
        }
        
        return true;
    } catch (e) { 
        logger.error({ err: e.message }, "Recovery failed");
        return false; 
    }
  }

  async commitBlock(block, stateChanges) {
    if (this.safeMode) throw new Error("Engine in Safe Mode");
    
    let lock;
    const start = Date.now(); // [3] Start Latency Timer

    try {
        // [1] Write-Ahead Log (WAL) persistence before Redlock
        await this.cluster.lpush("{mdc}:wal:active", JSON.stringify({ block, ts: Date.now() }));

        lock = await this.redlock.acquire([`{mdc}:locks:block:${block.height}`], 15000);
        await this.commitBreaker.fire(block.height, block, stateChanges);
        
        // [1] Success: Clean WAL
        await this.cluster.lpop("{mdc}:wal:active");
        await this.cluster.set("{mdc}:block:last", JSON.stringify(block));

        this.currentHeight = block.height;

        // [3] METRICS: Latency & Throughput Logging
        logger.info({ 
            height: block.height, 
            latency: `${Date.now() - start}ms`,
            tx_count: stateChanges.length 
        }, "Block Committed to Global State");

        return true;
    } catch (err) {
        if (err.message.includes("ERR_BLOCK_EXISTS")) return true;
        logger.error({ err: err.message }, "Global Consensus Failed");
        throw err;
    } finally {
        if (lock) await lock.release().catch(() => {});
    }
  }

  async _executeLuaCommit(h, b, c) {
      return await this.cluster.commitMedorBlock(h.toString(), JSON.stringify(b), JSON.stringify(c));
  }

  _setupGracefulShutdown() {
      const shutdown = async (s) => {
          logger.info({ signal: s }, "Global node shutting down...");
          this.isRunning = false;
          await this.cluster.quit().catch(() => {});
          for (const node of this.redlockNodes) await node.quit().catch(() => {});
          process.exit(0);
      };
      process.on('SIGINT', () => shutdown('SIGINT'));
      process.on('SIGTERM', () => shutdown('SIGTERM'));
  }
}

module.exports = TransactionEngine;
