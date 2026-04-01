const Redis = require('ioredis');
/** * THE EXACT PROBLEM FIX: 
 * Redlock v5+ requires .default when using CommonJS 'require'.
 * This ensures 'Redlock' is a constructor so Line 61 works.
 */
const Redlock = require('redlock').default || require('redlock');
const jwt = require('jsonwebtoken');
const axios = require('axios');
const os = require('os');

const logger = { 
    info: (msg) => console.log(`[MEDOR-INFO] ${new Date().toISOString()} - ${msg}`),
    error: (msg, src, err) => console.error(`[MEDOR-CRITICAL] ${src}: ${msg}`, err || "") 
};

class TransactionEngine {
  constructor(redisClients, secretKey, nodeId = 'node-01') {
    this.nodeId = nodeId;
    this.keys = { current: secretKey, previous: process.env.PREVIOUS_JWT_KEY || null };
    this.webhookUrl = process.env.ALERT_WEBHOOK_URL;
    
    // Infrastructure State
    this.isRunning = false;
    this.breaker = { tripped: false, failureCount: 0, threshold: 5, resetTime: 30000 };
    this.retryConfig = { maxRetries: 3, initialDelay: 500 };
    this.MAX_PARALLEL_WORKERS = process.env.MAX_WORKERS || os.cpus().length;
    this.lastSyncPublish = 0;

    const redisOptions = {
        retryStrategy: (times) => Math.min(times * 100, 3000),
        maxRetriesPerRequest: null,
        enableOfflineQueue: true 
    };

    // Redis Client Setup
    const inputClients = Array.isArray(redisClients) ? redisClients : [redisClients];
    this.redisClients = inputClients.filter(c => c && typeof c.on === 'function');

    if (this.redisClients.length === 0) {
        this.redis = new Redis(process.env.REDIS_URL || 'redis://127.0.0.1:6379', redisOptions);
        this.redisClients = [this.redis];
    } else {
        this.redis = this.redisClients[0];
    }

    // LINE 61: This will now work because Redlock was imported correctly above.
    this.redlock = new Redlock(this.redisClients, { 
        driftFactor: 0.01, 
        retryCount: 50, 
        retryDelay: 150 
    });

    // Atomic Lua Script for All-or-Nothing Commit
    this.redis.defineCommand('commitMedorBlock', {
        numberOfKeys: 0,
        lua: `
            local blockHeight = ARGV[1]
            local blockData = ARGV[2]
            local stateChanges = cjson.decode(ARGV[3])
            redis.call('HSET', 'mdc:blocks:' .. blockHeight, 'data', blockData)
            redis.call('HSET', 'mdc:meta:stats', 'currentHeight', blockHeight)
            for _, change in ipairs(stateChanges) do
                redis.call('HINCRBYFLOAT', 'user:' .. change.address .. ':state', change.field, change.delta)
            end
            return true
        `
    });

    this.currentHeight = 0;
    this.init();
  }

  async init() {
      try {
          this.isRunning = true;
          await this._reconcileState();
          this._startEventDrivenAlerts();
          this._startParallelProcessor();
          logger.info(`Sovereign Engine: ONLINE. Workers: ${this.MAX_PARALLEL_WORKERS}`);
      } catch (e) {
          logger.error("Boot Failure", "CORE", e.message);
      }
  }

  async shutdown() {
      logger.info("Graceful shutdown initiated...");
      this.isRunning = false;
      try {
          await Promise.all(this.redisClients.map(client => client.quit()));
          logger.info("Clean shutdown complete.");
      } catch (e) { logger.error("Shutdown Error", "CORE", e.message); }
  }

  // --- RELIABILITY HELPERS ---
  async _queueAlert(title, msg) {
      try {
          const payload = JSON.stringify({ nodeId: this.nodeId, title, msg });
          await this.redis.lpush("mdc:alerts", payload);
          await this.redis.ltrim("mdc:alerts", 0, 1000);
      } catch (e) { logger.error("Alert Queue Full", "STORAGE", e.message); }
  }

  async _logAudit(action, details) {
      try {
          const entry = JSON.stringify({ action, details, ts: Date.now(), node: this.nodeId });
          await this.redis.lpush("mdc:audit:log", entry);
          await this.redis.ltrim("mdc:audit:log", 0, 5000);
      } catch (e) { logger.error("Audit Log Error", "STORAGE", e.message); }
  }

  async _startEventDrivenAlerts() {
      const client = this.redis.duplicate();
      (async () => {
          while (this.isRunning) {
              try {
                  const data = await client.blpop("mdc:alerts", 5);
                  if (data && this.webhookUrl && !this.breaker.tripped) {
                      const alert = JSON.parse(data[1]);
                      await this._sendWebhookWithRetry(alert);
                  }
              } catch (e) { await new Promise(r => setTimeout(r, 5000)); }
          }
          client.quit();
      })();
  }

  async _sendWebhookWithRetry(alert, attempt = 1) {
      try {
          await axios.post(this.webhookUrl, { content: `🚨 **[${alert.nodeId}] ${alert.title}**\n${alert.msg}` });
      } catch (e) {
          if (attempt < 3) setTimeout(() => this._sendWebhookWithRetry(alert, attempt + 1), 5000);
      }
  }

  // --- WORKER CORE ---
  async enqueueBlock(block, stateChanges, token) {
      const payload = JSON.stringify({ block, stateChanges, token, ts: Date.now() });
      await this.redis.lpush("mdc:queue:pending", payload);
  }

  async _startParallelProcessor() {
      for (let i = 0; i < this.MAX_PARALLEL_WORKERS; i++) {
          this._runWorker(i);
      }
  }

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
              await this._updateMetric("blocks_processed");
              await this._logAudit("BLOCK_COMMIT", { height: block.height, worker: workerId });
              
              workerBackoff = 1000;
              this.breaker.failureCount = 0;
          } catch (e) {
              this._handleBreaker();
              logger.error(`Worker ${workerId} Error`, "QUEUE", e.message);
              await new Promise(r => setTimeout(r, workerBackoff));
              workerBackoff = Math.min(workerBackoff * 2, 30000);
          }
      }
  }

  async _executeWithBackoff(fn, blockHeight) {
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
          logger.error("CIRCUIT BREAKER TRIPPED", "SYSTEM", "Muting alerts and pausing queue.");
          setTimeout(() => {
              this.breaker.tripped = false;
              this.breaker.failureCount = 0;
          }, this.breaker.resetTime);
      }
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

  async _updateMetric(name, val = 1) {
      try { await this.redis.hincrby("mdc:metrics", `${this.nodeId}:${name}`, val); } catch(e){}
  }

  async commitBlock(block, stateChanges, token) {
    this._verifyToken(token);
    let lock;

    try {
        lock = await this.redlock.acquire([`locks:block:${block.height}`], 15000);
        
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

      async recoverFromCrash() {
        const lockKey = "mdc:recovery:lock";
        const ttl = 30000; // 30s for full consistency check
        
        this.logger.info(`[RECOVERY] [${this.nodeId}] Initializing WAL-based recovery...`);
        
        try {
            const lock = await this.redlock.acquire([lockKey], ttl);
            
            // 1. WAL REPLAY: Check for uncommitted writes in the Write-Ahead Log
            const walEntries = await this.redis.lrange("mdc:wal:active", 0, -1);
            if (walEntries.length > 0) {
                this.logger.warn(`[WAL] Found ${walEntries.length} uncommitted entries. Replaying...`);
                for (const entry of walEntries) {
                    const { tx, signature, type } = JSON.parse(entry);
                    
                    // VALIDATION: Reject if signature or balance is invalid during replay
                    if (!await this.validateTransaction(tx, signature)) {
                        this.logger.error(`[WAL] Invalid entry detected in log. Purging.`);
                        await this.redis.lrem("mdc:wal:active", 1, entry);
                        continue;
                    }
                    
                    await this._executeAtomicUpdate(tx, type); // Re-run the actual logic
                    await this.redis.lrem("mdc:wal:active", 1, entry);
                }
            }

            // 2. FULL CONSISTENCY CHECK: Last Block vs Redis State
            const lastBlock = await this.redis.get("mdc:block:last");
            const redisHeight = await this.redis.get("mdc:meta:height");

            if (lastBlock && JSON.parse(lastBlock).height !== parseInt(redisHeight)) {
                this.logger.fatal("STATE MISMATCH: Last block height does not match metadata. Entering SAFE MODE.");
                this.safeMode = true;
                return { status: "safe_mode", reason: "height_mismatch" };
            }

            // 3. RECONCILE MEMPOOL
            const processing = await this.redis.lrange("mdc:mempool:processing", 0, -1);
            if (processing.length > 0) {
                const pipeline = this.redis.multi();
                processing.forEach(tx => {
                    pipeline.lpush("mdc:mempool:main", tx);
                    pipeline.lrem("mdc:mempool:processing", 1, tx);
                });
                await pipeline.exec();
            }

            await lock.release();
            this.logger.info(`[RECOVERY] Success. Chain height: ${redisHeight}`);
            return { status: "recovered" };

        } catch (err) {
            if (err.name === 'ResourceLockedError') return { status: "concurrent_skip" };
            this.logger.error(`[FATAL] Recovery Exception: ${err.stack}`);
            throw new Error("RECOVERY_FAILED_SYSTEM_HALT");
        }
    }
