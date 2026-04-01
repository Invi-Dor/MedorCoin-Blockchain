// 1. REAL INDUSTRIAL DEPENDENCIES
const Redis = require('ioredis');
const Redlock = require('redlock');
const jwt = require('jsonwebtoken');
const axios = require('axios');

const logger = { 
    info: (msg) => console.log(`[MEDOR-INFO] ${msg}`),
    error: (msg, src, err) => console.error(`[MEDOR-CRITICAL] ${src}: ${msg}`, err || "") 
};

const metrics = { increment: () => {}, registerDefaults: () => {} };

class TransactionEngine {
  constructor(redisClients, secretKey, nodeId = 'node-01') {
    // POINT 4: ENV VALIDATION
    this.webhookUrl = process.env.ALERT_WEBHOOK_URL;
    if (!this.webhookUrl) logger.info("Warning: ALERT_WEBHOOK_URL not set. Alerts will be local-only.");

    // POINT 1 & 2: REDIS CLUSTER / REDLOCK HARDENING
    // Ensure redisClients is an array for Redlock quorum logic
    this.redisClients = Array.isArray(redisClients) ? redisClients : [redisClients];
    this.redis = this.redisClients[0]; 
    
    // POINT 5 & 6: SILENT CRASH PREVENTION
    this.redisClients.forEach(client => {
        client.on('error', (err) => logger.error("Redis Connection Error", "CORE", err));
    });

    this.secretKey = secretKey;
    this.keyKeys = {
      current: secretKey,
      previous: process.env.PREVIOUS_JWT_KEY || null,
    };
    this.nodeId = nodeId;

    this.commitQueue = [];
    this.maxQueueSize = 1000;
    this.isProcessingQueue = false;

    this.maxConcurrentAlerts = 5; 
    this.activeAlerts = 0;
    this.alertBackoff = 1000;

    this.metricsBuffer = new Map();
    this.metricsFlushInterval = 5000; 

    this.breakers = {
      redis: { tripped: false, failures: 0, threshold: 5 },
      validation: { tripped: false, failures: 0, threshold: 3 },
      network: { tripped: false, failures: 0, threshold: 10 }
    };

    // POINT 2: REDLOCK INITIALIZATION
    this.redlock = new Redlock(this.redisClients, {
      driftFactor: 0.01,
      retryCount: 60,
      retryDelay: 40,  
      retryJitter: 400,
      automaticExtensionThreshold: 600
    });

    this.init();
  }

  // RECOVERY PROTOCOL (Required by server.cjs)
  async recoverFromCrash() { 
      logger.info("Crash recovery protocol: Active. Reconciling state...");
      return await this._reconcileState(); 
  }

  async init() {
    try {
      await this._reconcileState();
      this._startHeartbeat();
      this._listenForClusterEvents();
      
      setInterval(() => this._enforceGlobalPlanExpiryBatched(), 300000); 
      setInterval(() => this._flushMetricsBuffer(), this.metricsFlushInterval);
      
      this._flushAlertQueue();
      metrics.registerDefaults();
      logger.info(`${this.nodeId} Engine Initialized.`);
    } catch (err) {
      logger.error("Initialization Failed", "CORE", err);
    }
  }

  // --- LOGIC METHODS ---

  async enqueueBlock(block, stateChanges, token) {
    if (this.commitQueue.length >= this.maxQueueSize) {
      throw new Error("System Overload: Commit Queue Full");
    }
    return new Promise((resolve, reject) => {
      this.commitQueue.push({ block, stateChanges, token, resolve, reject });
      this._processCommitQueue();
    });
  }

  async _processCommitQueue() {
    if (this.isProcessingQueue || this.commitQueue.length === 0) return;
    this.isProcessingQueue = true;
    while (this.commitQueue.length > 0) {
      const { block, stateChanges, token, resolve, reject } = this.commitQueue.shift();
      try {
        const result = await this.commitBlock(block, stateChanges, token);
        resolve(result);
      } catch (err) { reject(err); }
    }
    this.isProcessingQueue = false;
  }

  async commitBlock(block, stateChanges, token) {
    if (this.breakers.redis.tripped) throw new Error("Circuit Breaker: Redis Offline");

    // POINT 3: JWT VERIFICATION
    const decoded = this._verifyVersionedToken(token);
    if (!decoded.isProducer) throw new Error("Unauthorized Producer");

    if (block.height <= this.currentHeight) return true; 

    const userResources = stateChanges.map(c => `locks:user:${c.address}`);
    const lockResources = [`locks:block_height:${block.height}`, ...userResources];
    
    let lock;
    try {
      lock = await this.redlock.acquire(lockResources, 15000);
      
      const chainStats = await this.redis.hgetall("mdc:meta:stats");
      if (chainStats && parseInt(chainStats.currentHeight) >= block.height) return true; 

      const snapshot = await this._takeStateSnapshotPipelined(stateChanges);
      const pipeline = this.redis.multi();
      
      pipeline.hset(`mdc:blocks:${block.height}`, 'data', JSON.stringify(block));
      pipeline.hset("mdc:meta:stats", "lastBlockHash", block.hash);
      pipeline.hset("mdc:meta:stats", "currentHeight", block.height.toString());

      for (const change of stateChanges) {
        const userKey = `user:${change.address}:state`;
        pipeline.hsetnx(userKey, "balance", "0");
        pipeline.hsetnx(userKey, "hashrate", "0");
        pipeline.hincrbyfloat(userKey, change.field, parseFloat(change.delta) || 0);
      }

      const results = await pipeline.exec();
      if (!results || results.some(r => r[0])) {
        await this._rollbackState(snapshot);
        throw new Error("Pipeline Execution Failure");
      }

      this.currentHeight = block.height;
      this.lastBlockHash = block.hash;
      this._resetBreaker('validation');
      this._broadcast('block_committed', { height: block.height });

      return true;
    } catch (err) {
      this._handleFailure('validation', "COMMIT_FAILED", err.message);
      throw err;
    } finally {
      if (lock) await lock.release().catch(() => {});
    }
  }

  _verifyVersionedToken(token) {
    try { return jwt.verify(token, this.keyKeys.current); } 
    catch (e) {
      if (this.keyKeys.previous) return jwt.verify(token, this.keyKeys.previous);
      throw new Error("Invalid or Expired Security Token");
    }
  }

  async _reconcileState() {
    try {
      const stats = await this.redis.hgetall("mdc:meta:stats");
      this.currentHeight = parseInt(stats.currentHeight) || 0;
      this.lastBlockHash = stats.lastBlockHash || "0".repeat(64);
      return true;
    } catch (e) { 
      this._handleFailure('redis', "RECONCILE_FAIL", e.message); 
      return false;
    }
  }

  _broadcast(event, data) { 
      this.redis.publish("mdc:cluster:events", JSON.stringify({ nodeId: this.nodeId, event, data }))
          .catch(err => logger.error("PubSub Broadcast Failed", "NETWORK", err));
  }

  _listenForClusterEvents() {
    const sub = this.redis.duplicate(); 
    sub.on('error', (err) => logger.error("Cluster Subscriber Error", "NETWORK", err));
    sub.subscribe("mdc:cluster:events");
    sub.on("message", (chan, msg) => {
      try {
        const e = JSON.parse(msg); 
        if (e.event === 'block_committed' && e.data.height > this.currentHeight) this.currentHeight = e.data.height;
      } catch (err) { logger.error("Malformed Cluster Message", "NETWORK", err); }
    });
  }

  // ... (Other internal utilities: _flushAlertQueue, _enforceGlobalPlanExpiryBatched, etc. remain here)
}

module.exports = TransactionEngine;
