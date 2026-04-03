const Redis = require('ioredis');
const Redlock = require('redlock').default || require('redlock');
const jwt = require('jsonwebtoken');
const axios = require('axios');
const os = require('os');
const crypto = require('crypto');
const secp256k1 = require('secp256k1');
const pino = require('pino');
const CircuitBreaker = require('opossum');

const logger = pino({
    level: process.env.LOG_LEVEL || 'info',
    transport: { target: 'pino-pretty' }
});

    /**
     * PRODUCTION CLUSTER CONFIGURATION
     * Connects to your 3 local Redis nodes on ports 6379, 6380, and 6381.
     */
    this.redisClients = [
        new Redis(6379, "127.0.0.1", redisOptions),
        new Redis(6380, "127.0.0.1", redisOptions),
        new Redis(6381, "127.0.0.1", redisOptions)
    ];

    // Primary command interface (uses the first node)
    this.redis = this.redisClients[0];

    // GLOBAL REDLOCK: Distributed consensus across all 3 nodes.
    // This fixes the "split-brain" warning.
    this.redlock = new Redlock(this.redisClients, { 
        driftFactor: 0.01, 
        retryCount: 50, 
        retryDelay: 150 
    });

    /**
     * INFRASTRUCTURE NOTE: Redlock Drift
     * For true distributed safety, ensure all Redis nodes in this array 
     * have their system clocks synchronized via NTP (Network Time Protocol).
     */
    const inputClients = Array.isArray(redisClients) ? redisClients : [redisClients];
    this.redisClients = inputClients.filter(c => c && typeof c.on === 'function');

    if (this.redisClients.length === 0) {
        logger.warn("WARNING: Running Redlock with a single instance. Vulnerable to split-brain.");
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

    /**
     * LUA CORE: Atomic, Idempotent, Integer-Based Commit
     * Switched from HINCRBYFLOAT to HINCRBY (64-bit signed integers).
     * Uses redis.error_reply to natively throw JS errors in ioredis.
     */
    this.redis.defineCommand('commitMedorBlock', {
        numberOfKeys: 0,
        lua: `
            local blockHeight = ARGV[1]
            local blockData = ARGV[2]
            local stateChanges = cjson.decode(ARGV[3])
            
            if redis.call('HEXISTS', 'mdc:blocks:' .. blockHeight, 'data') == 1 then
                return redis.error_reply("IDEMPOTENCY_VIOLATION: Block already committed")
            end

            redis.call('SET', 'mdc:block:last', blockData)
            redis.call('HSET', 'mdc:blocks:' .. blockHeight, 'data', blockData)
            redis.call('SET', 'mdc:meta:height', blockHeight)
            redis.call('HSET', 'mdc:meta:stats', 'currentHeight', blockHeight)

            for _, change in ipairs(stateChanges) do
                -- amount and delta MUST be passed as integer representations (e.g. cents/satoshis)
                redis.call('HINCRBY', 'mdc:balances', change.address, change.delta)
            end
            return "OK"
        `
    });

    this.commitBreaker = new CircuitBreaker(this._executeLuaCommit.bind(this), {
        timeout: 5000, 
        errorThresholdPercentage: 50, 
        resetTimeout: 30000 
    });
    
    this.commitBreaker.on('open', () => logger.fatal('CIRCUIT TRIPPED: Halting block commits.'));
    this.commitBreaker.on('halfOpen', () => logger.warn('CIRCUIT HALF-OPEN: Testing database stability.'));
    this.commitBreaker.on('close', () => logger.info('CIRCUIT CLOSED: Resuming normal operations.'));

    this.currentHeight = 0;
    this.init();
  }

  async init() {
      try {
          this.isRunning = true;
          await this.recoverFromCrash();
          this._startEventDrivenAlerts();
          this._startParallelProcessor();
          logger.info({ workers: this.MAX_PARALLEL_WORKERS }, 'MedorCoin Engine: ONLINE');
      } catch (e) {
          logger.error({ err: e }, "Boot Failure");
      }
  }

  // --- CRYPTOGRAPHY & BIGINT MATH ---
  async validateTransaction(tx, signature) {
    try {
        if (this.safeMode) return false;
        
        // Use BigInt to eliminate floating point imprecision
        const balance = await this.redis.hget("mdc:balances", tx.sender);
        const currentBalance = BigInt(balance || 0);
        const requiredAmount = BigInt(tx.amount) + BigInt(tx.fee || 0);
        
        if (currentBalance < requiredAmount) return false;

        const msgHash = crypto.createHash('sha256').update(JSON.stringify(tx)).digest();
        const sigBuffer = Buffer.from(signature, 'hex');
        const pubKeyBuffer = Buffer.from(tx.senderPublicKey, 'hex');
        return secp256k1.ecdsaVerify(sigBuffer, msgHash, pubKeyBuffer);
    } catch (e) { return false; }
  }

  // --- RECOVERY ---
  async recoverFromCrash() {
    const lockKey = "mdc:recovery:lock";
    logger.info(`[${this.nodeId}] Initializing WAL recovery...`);
    try {
        const lock = await this.redlock.acquire([lockKey], 30000);
        
        const walEntries = await this.redis.lrange("mdc:wal:active", 0, -1);
        if (walEntries.length > 0) {
            logger.warn({ count: walEntries.length }, 'Replaying uncommitted WAL entries.');
            for (const entry of walEntries) {
                const { block, stateChanges, token } = JSON.parse(entry);
                try {
                    await this.commitBlock(block, stateChanges, token);
                    await this.redis.lrem("mdc:wal:active", 1, entry);
                } catch (e) { logger.error({ err: e }, "WAL Replay Failed"); }
            }
        }

        const lastBlockRaw = await this.redis.get("mdc:block:last");
        const redisHeight = await this.redis.get("mdc:meta:height");
        if (lastBlockRaw && JSON.parse(lastBlockRaw).height !== parseInt(redisHeight || 0)) {
            logger.fatal("STATE MISMATCH: Chain integrity compromised. Entering SAFE MODE.");
            this.safeMode = true;
            return { status: "safe_mode" };
        }

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
        throw err;
    }
  }

  // --- CORE WORKER ---
  async commitBlock(block, stateChanges, token) {
    this._verifyToken(token);
    let lock;
    try {
        lock = await this.redlock.acquire([`locks:block:${block.height}`], 15000);
        
        // This will now properly throw an exception if Lua returns an error_reply
        await this.commitBreaker.fire(block.height, block, stateChanges);

        this.currentHeight = block.height;
        if (Date.now() - this.lastSyncPublish > 1000) {
            this.redis.publish("mdc:cluster:sync", JSON.stringify({ h: block.height }));
            this.lastSyncPublish = Date.now();
        }
        return true;
    } catch (err) {
        await this._queueAlert("COMMIT_ERROR", err.message);
        throw err;
    } finally {
        if (lock) await lock.release().catch(() => {});
    }
  }

  async _executeLuaCommit(height, block, stateChanges) {
      // Passes stringified integers to avoid JS parsing float conversion before it hits Redis
      return await this.redis.commitMedorBlock(
          height.toString(),
          JSON.stringify(block),
          JSON.stringify(stateChanges)
      );
  }

  async _reconcileState() {
      const h = await this.redis.get("mdc:meta:height");
      this.currentHeight = parseInt(h) || 0;
  }

  _verifyToken(token) {
    try { return jwt.verify(token, this.keys.current); } 
    catch (e) { throw new Error("Security Violation: Token Refused"); }
  }

  async _runWorker(workerId) {
      let workerBackoff = 1000;
      while (this.isRunning) {
          try {
              const data = await this.redis.brpoplpush("mdc:queue:pending", "mdc:queue:processing", 5);
              if (!data) continue;

              const { block, stateChanges, token } = JSON.parse(data);
              await this.commitBlock(block, stateChanges, token);
              
              await this.redis.lrem("mdc:queue:processing", 1, data);
              workerBackoff = 1000;
          } catch (e) {
              logger.error({ workerId, err: e }, 'Worker Queue Error');
              await new Promise(r => setTimeout(r, workerBackoff));
              workerBackoff = Math.min(workerBackoff * 2, 30000);
          }
      }
  }

  async _startParallelProcessor() {
      for (let i = 0; i < this.MAX_PARALLEL_WORKERS; i++) {
          this._runWorker(i);
      }
  }

  async _startEventDrivenAlerts() {
      this.alertClient = this.redis.duplicate();
      while (this.isRunning) {
          try {
              const data = await this.alertClient.blpop("mdc:alerts", 5);
              if (data && this.webhookUrl) {
                  await axios.post(this.webhookUrl, { content: `🚨 Alert: ${data[1]}` });
              }
          } catch (e) { await new Promise(r => setTimeout(r, 5000)); }
      }
  }

  async shutdown() {
      this.isRunning = false;
      if (this.alertClient) await this.alertClient.quit();
      await Promise.all(this.redisClients.map(client => client.quit()));
  }

  async _queueAlert(title, msg) {
      try { await this.redis.lpush("mdc:alerts", JSON.stringify({ nodeId: this.nodeId, title, msg })); } 
      catch (e) {}
  }
}

module.exports = TransactionEngine;
