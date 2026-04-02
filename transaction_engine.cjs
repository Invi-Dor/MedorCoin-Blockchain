const Redis = require('ioredis');
/** * Redlock v5+ requires .default when using CommonJS 'require'.
 */
const Redlock = require('redlock').default || require('redlock');
const jwt = require('jsonwebtoken');
const axios = require('axios');
const os = require('os');
const crypto = require('crypto');
const secp256k1 = require('secp256k1'); // Industrial-grade ECC

// Structured Global Logger
const logger = { 
    info: (msg) => console.log(`[MEDOR-INFO] ${new Date().toISOString()} - ${msg}`),
    warn: (msg) => console.warn(`[MEDOR-WARN] ${new Date().toISOString()} - ${msg}`),
    error: (msg, src, err) => console.error(`[MEDOR-CRITICAL] ${src}: ${msg}`, err || ""),
    fatal: (msg) => {
        console.error(`[FATAL-HALT] ${new Date().toISOString()} - ${msg}`);
        process.emit('SIGTERM'); // Signal for container orchestration to restart
    }
};

class TransactionEngine {
  constructor(redisClients, secretKey, nodeId = 'node-01') {
    this.nodeId = nodeId;
    this.keys = { current: secretKey, previous: process.env.PREVIOUS_JWT_KEY || null };
    this.webhookUrl = process.env.ALERT_WEBHOOK_URL;
    this.logger = logger; // Mapping internal references to the global logger
    
    this.isRunning = false;
    this.safeMode = false;
    this.breaker = { tripped: false, failureCount: 0, threshold: 5, resetTime: 30000 };
    this.retryConfig = { maxRetries: 3, initialDelay: 500 };
    this.MAX_PARALLEL_WORKERS = process.env.MAX_WORKERS || os.cpus().length;
    this.lastSyncPublish = 0;

    const redisOptions = {
        retryStrategy: (times) => Math.min(times * 100, 3000),
        maxRetriesPerRequest: null,
        enableOfflineQueue: true 
    };

    const inputClients = Array.isArray(redisClients) ? redisClients : [redisClients];
    this.redisClients = inputClients.filter(c => c && typeof c.on === 'function');

    if (this.redisClients.length === 0) {
        this.redis = new Redis(process.env.REDIS_URL || 'redis://127.0.0.1:6379', redisOptions);
        this.redisClients = [this.redis];
    } else {
        this.redis = this.redisClients[0];
    }

    this.redlock = new Redlock(this.redisClients, { 
        driftFactor: 0.01, 
        retryCount: 50, 
        retryDelay: 150 
    });

    // LUA: The "Single Source of Truth" for State Transitions
    this.redis.defineCommand('commitMedorBlock', {
        numberOfKeys: 0,
        lua: `
            local blockHeight = ARGV[1]
            local blockData = ARGV[2]
            local stateChanges = cjson.decode(ARGV[3])
            
            -- Prevent Replay/Double Commit
            local current = redis.call('GET', 'mdc:meta:height')
            if current and tonumber(blockHeight) <= tonumber(current) then
                return {err = "BLOCK_ALREADY_COMMITTED"}
            end

            redis.call('SET', 'mdc:block:last', blockData)
            redis.call('HSET', 'mdc:blocks:' .. blockHeight, 'data', blockData)
            redis.call('SET', 'mdc:meta:height', blockHeight)
            redis.call('HSET', 'mdc:meta:stats', 'currentHeight', blockHeight)

            for _, change in ipairs(stateChanges) do
                redis.call('HINCRBYFLOAT', 'mdc:balances', change.address, change.delta)
            end
            return true
        `
    });

    this.currentHeight = 0;
    this.init();
  }

  // --- CRYPTOGRAPHIC VALIDATION ---
  async validateTransaction(tx, signature) {
    try {
        if (this.safeMode) return false;
        
        // 1. Double-Spend Check (Industrial Scale)
        const balance = await this.redis.hget("mdc:balances", tx.sender);
        if (parseFloat(balance || 0) < (tx.amount + (tx.fee || 0))) return false;

        // 2. Cryptographic Signature Verification (secp256k1)
        const msgHash = crypto.createHash('sha256').update(JSON.stringify(tx)).digest();
        const sigBuffer = Buffer.from(signature, 'hex');
        const pubKeyBuffer = Buffer.from(tx.senderPublicKey, 'hex');
        
        return secp256k1.ecdsaVerify(sigBuffer, msgHash, pubKeyBuffer);
    } catch (e) {
        this.logger.error("Validation Error", "CRYPTO", e.message);
        return false;
    }
  }

  async init() {
      try {
          this.isRunning = true;
          await this.recoverFromCrash();
          this._startEventDrivenAlerts();
          this._startParallelProcessor();
          this.logger.info(`Sovereign Engine: ONLINE. Workers: ${this.MAX_PARALLEL_WORKERS}`);
      } catch (e) {
          this.logger.error("Boot Failure", "CORE", e.message);
      }
  }

  async recoverFromCrash() {
    const lockKey = "mdc:recovery:lock";
    const ttl = 30000;
    this.logger.info(`[RECOVERY] [${this.nodeId}] Initializing WAL-based recovery...`);
    
    try {
        const lock = await this.redlock.acquire([lockKey], ttl);
        
        // 1. WAL REPLAY (Write-Ahead Log Re-processing)
        const walEntries = await this.redis.lrange("mdc:wal:active", 0, -1);
        if (walEntries.length > 0) {
            this.logger.warn(`[WAL] Found ${walEntries.length} uncommitted entries. Replaying...`);
            for (const entry of walEntries) {
                const { block, stateChanges, token } = JSON.parse(entry);
                try {
                    await this.commitBlock(block, stateChanges, token);
                    await this.redis.lrem("mdc:wal:active", 1, entry);
                } catch (e) {
                    this.logger.error("WAL Replay Item Failed", "RECOVERY", e.message);
                }
            }
        }

        // 2. FULL CONSISTENCY CHECK
        const lastBlockRaw = await this.redis.get("mdc:block:last");
        const redisHeight = await this.redis.get("mdc:meta:height");

        if (lastBlockRaw) {
            const lastBlock = JSON.parse(lastBlockRaw);
            if (lastBlock.height !== parseInt(redisHeight || 0)) {
                this.logger.fatal("STATE MISMATCH: Chain integrity compromised. Entering SAFE MODE.");
                this.safeMode = true;
                return { status: "safe_mode" };
            }
        }

        // 3. RECONCILE MEMPOOL (Return processing items to main queue)
        const processing = await this.redis.lrange("mdc:queue:processing", 0, -1);
        if (processing.length > 0) {
            const pipeline = this.redis.multi();
            processing.forEach(tx => {
                pipeline.lpush("mdc:queue:pending", tx);
                pipeline.lrem("mdc:queue:processing", 1, tx);
            });
            await pipeline.exec();
        }

        await this._reconcileState();
        await lock.release();
        return { status: "recovered" };

    } catch (err) {
        if (err.name === 'ResourceLockedError') return { status: "concurrent_skip" };
        this.logger.error("Recovery Exception", "HALT", err.stack);
        throw err;
    }
  }

  async commitBlock(block, stateChanges, token) {
    this._verifyToken(token);
    let lock;

    try {
        lock = await this.redlock.acquire([`locks:block:${block.height}`], 15000);
        
        // Industrial Atomic Commit via LUA
        await this._executeWithBackoff(async () => {
            await this.redis.commitMedorBlock(
                block.height.toString(),
                JSON.stringify(block),
                JSON.stringify(stateChanges)
            );
        }, block.height);

        this.currentHeight = block.height;
        
        const now = Date.now();
        if (now - this.lastSyncPublish > 1000) {
            this.redis.publish("mdc:cluster:sync", JSON.stringify({ h: block.height }));
            this.lastSyncPublish = now;
        }
        
        return true;
    } catch (err) {
        await this._queueAlert("LUA_COMMIT_ERROR", err.message);
        throw err;
    } finally {
        if (lock) await lock.release().catch(() => {});
    }
  }

  async _reconcileState() {
      const h = await this.redis.get("mdc:meta:height");
      this.currentHeight = parseInt(h) || 0;
  }

  _verifyToken(token) {
    try { return jwt.verify(token, this.keys.current); } 
    catch (e) {
        if (this.keys.previous) {
            try { return jwt.verify(token, this.keys.previous); } catch (e2) {}
        }
        throw new Error("Security Violation: Token Refused");
    }
  }

  // --- REST OF INFRASTRUCTURE ---
  async _runWorker(workerId) {
      let workerBackoff = 1000;
      while (this.isRunning) {
          if (this.breaker.tripped) {
              await new Promise(r => setTimeout(r, 5000));
              continue;
          }
          try {
              const data = await this.redis.brpoplpush("mdc:queue:pending", "mdc:queue:processing", 5);
              if (!data) continue;

              const { block, stateChanges, token } = JSON.parse(data);
              await this.commitBlock(block, stateChanges, token);
              
              await this.redis.lrem("mdc:queue:processing", 1, data);
              this.breaker.failureCount = 0;
          } catch (e) {
              this._handleBreaker();
              this.logger.error(`Worker ${workerId} Error`, "QUEUE", e.message);
              await new Promise(r => setTimeout(r, workerBackoff));
              workerBackoff = Math.min(workerBackoff * 2, 30000);
          }
      }
  }

  async _executeWithBackoff(fn) {
      let delay = this.retryConfig.initialDelay;
      for (let i = 0; i < this.retryConfig.maxRetries; i++) {
          try { return await fn(); } catch (err) {
              if (i === this.retryConfig.maxRetries - 1) throw err;
              await new Promise(r => setTimeout(r, delay));
              delay *= 2;
          }
      }
  }

  _handleBreaker() {
      this.breaker.failureCount++;
      if (this.breaker.failureCount >= this.breaker.threshold && !this.breaker.tripped) {
          this.breaker.tripped = true;
          this.logger.error("CIRCUIT BREAKER TRIPPED", "SYSTEM", "Muting alerts and pausing queue.");
          setTimeout(() => {
              this.breaker.tripped = false;
              this.breaker.failureCount = 0;
          }, this.breaker.resetTime);
      }
  }

  async shutdown() {
      this.isRunning = false;
      await Promise.all(this.redisClients.map(client => client.quit()));
  }

  async _queueAlert(title, msg) {
      try {
          const payload = JSON.stringify({ nodeId: this.nodeId, title, msg });
          await this.redis.lpush("mdc:alerts", payload);
      } catch (e) {}
  }

  async _startEventDrivenAlerts() {
      const client = this.redis.duplicate();
      (async () => {
          while (this.isRunning) {
              try {
                  const data = await client.blpop("mdc:alerts", 5);
                  if (data && this.webhookUrl && !this.breaker.tripped) {
                      await axios.post(this.webhookUrl, { content: `🚨 **[${this.nodeId}] Alert**\n${data[1]}` });
                  }
              } catch (e) { await new Promise(r => setTimeout(r, 5000)); }
          }
          client.quit();
      })();
  }

  async _startParallelProcessor() {
      for (let i = 0; i < this.MAX_PARALLEL_WORKERS; i++) {
          this._runWorker(i);
      }
  }
}

module.exports = TransactionEngine;
