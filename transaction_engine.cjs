const Redis = require('ioredis');
const Redlock = require('redlock');
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
    
    // 1. STATE & SHUTDOWN FLAGS
    this.isRunning = false;
    this.breaker = { tripped: false, failureCount: 0, threshold: 5, resetTime: 30000 };
    this.retryConfig = { maxRetries: 3, initialDelay: 500 };
    this.MAX_PARALLEL_WORKERS = process.env.MAX_WORKERS || os.cpus().length;
    this.lastSyncPublish = 0;

    // 2. REDIS INITIALIZATION
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

    // 3. ATOMIC LUA SCRIPT DEFINITION
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

    this.redlock = new Redlock(this.redisClients, { driftFactor: 0.01, retryCount: 50, retryDelay: 150 });
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

  // --- GRACEFUL SHUTDOWN (Solves Point 1) ---
  async shutdown() {
      logger.info("Graceful shutdown initiated...");
      this.isRunning = false; // Signals all while() loops to exit on their next cycle
      try {
          await Promise.all(this.redisClients.map(client => client.quit()));
          logger.info("Database connections closed cleanly.");
      } catch (e) {
          logger.error("Shutdown Error", "CORE", e.message);
      }
  }

  // --- ALERT QUEUE CAPPING (Solves Point 3) ---
  async _queueAlert(title, msg) {
      try {
          const payload = JSON.stringify({ nodeId: this.nodeId, title, msg });
          await this.redis.lpush("mdc:alerts", payload);
          await this.redis.ltrim("mdc:alerts", 0, 1000); // Cap queue at 1000 items
      } catch (e) {
          logger.error("Alert Queue Full/Failed", "STORAGE", e.message);
      }
  }

  async _logAudit(action, details) {
      try {
          const entry = JSON.stringify({ action, details, ts: Date.now(), node: this.nodeId });
          await this.redis.lpush("mdc:audit:log", entry);
          await this.redis.ltrim("mdc:audit:log", 0, 5000);
      } catch (e) { logger.error("Audit Log Failed", "STORAGE", e.message); }
  }

  async _startEventDrivenAlerts() {
      const client = this.redis.duplicate();
      (async () => {
          while (this.isRunning) {
              try {
                  const data = await client.blpop("mdc:alerts", 5); // 5s timeout allows checking this.isRunning
                  if (data && this.webhookUrl && !this.breaker.tripped) {
                      const alert = JSON.parse(data[1]);
                      await this._sendWebhookWithRetry(alert);
                  }
              } catch (e) { await new Promise(r => setTimeout(r, 5000)); }
          }
          client.quit(); // Clean up duplicate client on shutdown
      })();
  }

  async _sendWebhookWithRetry(alert, attempt = 1) {
      try {
          await axios.post(this.webhookUrl, { content: `🚨 **[${alert.nodeId}] ${alert.title}**\n${alert.msg}` });
      } catch (e) {
          if (attempt < 3) setTimeout(() => this._sendWebhookWithRetry(alert, attempt + 1), 5000);
      }
  }

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
              logger.error(`Worker ${workerId} Exhausted Retries`, "QUEUE", e.message);
              await new Promise(r => setTimeout(r, workerBackoff));
              workerBackoff = Math.min(workerBackoff * 2, 30000);
          }
      }
  }

  async _executeWithBackoff(fn, blockHeight) {
      let delay = this.retryConfig.initialDelay;
      for (let i = 0; i < this.retryConfig.maxRetries; i++) {
          try {
              return await fn();
          } catch (err) {
              if (i === this.retryConfig.maxRetries - 1) throw err;
              logger.info(`Lua Retry ${i + 1}/${this.retryConfig.maxRetries} for block ${blockHeight} in ${delay}ms...`);
              await new Promise(r => setTimeout(r, delay));
              delay *= 2;
          }
      }
  }

  _handleBreaker() {
      this.breaker.failureCount++;
      if (this.breaker.failureCount >= this.breaker.threshold && !this.breaker.tripped) {
          this.breaker.tripped = true;
          logger.error("CIRCUIT BREAKER TRIPPED", "SYSTEM", "Muting alerts and pausing workers.");
          setTimeout(() => {
              this.breaker.tripped = false;
              this.breaker.failureCount = 0;
              logger.info("Circuit Breaker Reset: Resuming processing.");
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

  // --- VISIBLE METRICS ERROR HANDLING (Solves Point 4) ---
  async _updateMetric(name, val = 1) {
      try { 
          await this.redis.hincrby("mdc:metrics", `${this.nodeId}:${name}`, val); 
      } catch(e) {
          logger.error(`Failed to record metric [${name}]`, "METRICS", e.message);
      }
  }

  async commitBlock(block, stateChanges, token) {
    this._verifyToken(token);
    let lock;

    try {
        lock = await this.redlock.acquire([`locks:block:${block.height}`], 15000);
        
        // --- EXPLICIT LUA BACKOFF (Solves Point 2) ---
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
      const h = await this.redis.hget("mdc:meta:stats", "currentHeight");
      this.currentHeight = parseInt(h) || 0;
  }
}

module.exports = TransactionEngine;
