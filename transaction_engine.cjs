const Redis = require('ioredis');
const Redlock = require('redlock').default || require('redlock');
const jwt = require('jsonwebtoken');
const os = require('os');
const crypto = require('crypto');
const secp256k1 = require('secp256k1');
const pino = require('pino');
const CircuitBreaker = require('opossum');

const logger = pino({
    level: process.env.LOG_LEVEL || 'info',
    base: { node_id: process.env.NODE_ID || 'global-node-01' },
    transport: { target: 'pino-pretty' }
});

class TransactionEngine {
  constructor(secretKey, nodeId) {
    this.nodeId = nodeId || `node-${crypto.randomBytes(4).toString('hex')}`;
    this.secret = secretKey;
    this.isRunning = false;
    this.currentHeight = 0;

    const redisOptions = {
        retryStrategy: (times) => Math.min(times * 100, 3000),
        maxRetriesPerRequest: null,
        enableOfflineQueue: true,
        connectTimeout: 10000
    };

    // 1. HARDENED INFRA: Real Cluster Client
    this.cluster = new Redis.Cluster([
        { port: 6379, host: "127.0.0.1" },
        { port: 6380, host: "127.0.0.1" },
        { port: 6381, host: "127.0.0.1" }
    ], { redisOptions, scaleReads: "slave", canRetryRouteCommand: true });

    // 2. HARDENED REDLOCK: Passing individual node clients for full quorum reliability
    this.nodes = [
        new Redis(6379, "127.0.0.1", redisOptions),
        new Redis(6380, "127.0.0.1", redisOptions),
        new Redis(6381, "127.0.0.1", redisOptions)
    ];
    this.redlock = new Redlock(this.nodes, { 
        driftFactor: 0.01, 
        retryCount: 30, 
        retryDelay: 200,
        retryJitter: 200 
    });

    // 3. ATOMIC LUA ENGINE: With Specific Error Parsing
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

    this.commitBreaker = new CircuitBreaker(this._executeLuaCommit.bind(this), {
        timeout: 5000, 
        errorThresholdPercentage: 50, 
        resetTimeout: 15000 
    });

    this.init();
  }

  // 4. PRODUCTION VALIDATION: Signature & Balance Checks
  async validateTransaction(tx, signature) {
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
          const h = await this.cluster.get("{mdc}:meta:height");
          this.currentHeight = parseInt(h) || 0;
          logger.info({ 
              infra: "3-NODE-CLUSTER",
              height: this.currentHeight,
              node: this.nodeId
          }, 'MEDORCOIN ENGINE: ONLINE (Sovereign Hardened)');
      } catch (e) {
          logger.fatal({ err: e.message }, "Engine Boot Failure");
      }
  }

  async commitBlock(block, stateChanges) {
    let lock;
    const start = Date.now();
    try {
        lock = await this.redlock.acquire([`{mdc}:locks:block:${block.height}`], 5000);
        await this.commitBreaker.fire(block.height, block, stateChanges);
        this.currentHeight = block.height;

        // METRICS: Tracking throughput
        logger.info({ 
            height: block.height, 
            latency_ms: Date.now() - start,
            tx_count: stateChanges.length 
        }, "Block Committed");
        return true;
    } catch (err) {
        if (err.message.includes("ERR_BLOCK_EXISTS")) {
            logger.warn({ height: block.height }, "Idempotency trigger: Block already in state");
            return true;
        }
        logger.error({ err: err.message }, "Global Commit Failure");
        throw err;
    } finally {
        if (lock) await lock.release().catch(() => {});
    }
  }

  async _executeLuaCommit(h, b, c) {
      return await this.cluster.commitMedorBlock(h.toString(), JSON.stringify(b), JSON.stringify(c));
  }
}

module.exports = TransactionEngine;
