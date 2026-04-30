/**
 * TRANSACTION_ENGINE.CJS - Execution & Validation Layer
 * Integrated with HybridDB (RocksDB + Redis Standalone)
 */

"use strict";

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
});

class TransactionEngine {
  constructor(dbInstance, nodeId) {
    this.db = dbInstance; // Integrated HybridDB (Step 1 Storage)
    this.nodeId = nodeId || `node-${crypto.randomBytes(4).toString('hex')}`;
    this.isRunning = false;
    this.safeMode = false;

    // 1. STANDALONE REDIS (Matches db.cjs and your server PONG)
    this.redis = new Redis({
        host: "127.0.0.1",
        port: 6379,
        retryStrategy: (times) => Math.min(times * 150, 5000)
    });

    this.redis.on('error', (err) => {
        logger.error({ err: err.message }, 'Redis Connection Error');
    });

    // 2. REDLOCK (Single Node Quorum for Standalone)
    this.redlock = new Redlock([this.redis], { 
        driftFactor: 0.01, 
        retryCount: 10, 
        retryDelay: 200 
    });

    // 3. TRANSACTION NONCE SYSTEM
    // (We will pull these from this.db.getNonce in the validation)

    this.commitBreaker = new CircuitBreaker(this._executeStateCommit.bind(this), {
        timeout: 10000, 
        errorThresholdPercentage: 50, 
        resetTimeout: 30000 
    });

    this._setupGracefulShutdown();
  } 

  /**
   * STEP 2 & 4: FULL TRANSACTION VALIDATION
   * Checks: Signature, Balance, and Nonce
   */
  async validateTransaction(tx, signature) {
    if (this.safeMode || !this.isRunning) return false;
    if (!tx.from || !tx.to || !tx.amount || !tx.nonce || !tx.publicKey) return false;

    try {
        // A. Check Nonce (Prevent Replay Attack)
        const currentNonce = await this.db.getNonce(tx.from);
        if (tx.nonce !== currentNonce) {
            logger.warn({ tx: tx.from, expected: currentNonce, got: tx.nonce }, "Invalid Nonce");
            return false;
        }

        // B. Check Balance
        const balance = await this.db.getBalance(tx.from);
        if (balance < BigInt(tx.amount)) {
            logger.warn({ user: tx.from, bal: balance.toString(), req: tx.amount }, "Insufficient Funds");
            return false;
        }

        // C. Verify secp256k1 Signature
        const msgHash = crypto.createHash('sha256').update(JSON.stringify({
            from: tx.from,
            to: tx.to,
            amount: tx.amount,
            nonce: tx.nonce
        })).digest();

        return secp256k1.ecdsaVerify(
            Buffer.from(signature, 'hex'), 
            msgHash, 
            Buffer.from(tx.publicKey, 'hex')
        );
    } catch (e) { 
        logger.error({ err: e.message }, "Validation Error");
        return false; 
    }
  }

  /**
   * STEP 5: STATE TRANSITION & STORAGE
   * Atomic update to RocksDB and Redis
   */
  async commitBlock(block, transactions) {
    if (this.safeMode) throw new Error("Engine in Safe Mode");
    
    let lock;
    const start = Date.now();

    try {
        lock = await this.redlock.acquire([`{mdr}:locks:block:${block.height}`], 5000);
        
        // Execute all state changes in a single batch
        const batchOps = [];
        for (const tx of transactions) {
            const fromBal = await this.db.getBalance(tx.from);
            const toBal = await this.db.getBalance(tx.to);
            const amount = BigInt(tx.amount);

            batchOps.push({ type: 'put', key: `st:${tx.from.toLowerCase()}`, value: (fromBal - amount).toString() });
            batchOps.push({ type: 'put', key: `st:${tx.to.toLowerCase()}`, value: (toBal + amount).toString() });
            batchOps.push({ type: 'put', key: `no:${tx.from.toLowerCase()}`, value: (tx.nonce + 1).toString() });
        }

        // Permanent save to RocksDB via HybridDB
        await this.db.batch(batchOps);
        await this.db.put(`ch:height`, block.height);
        await this.db.put(`ch:last_block`, block);

        logger.info({ 
            height: block.height, 
            latency: `${Date.now() - start}ms`,
            tx_count: transactions.length 
        }, "Block Validated and Stored");

        return true;
    } catch (err) {
        logger.error({ err: err.message }, "State Transition Failed");
        throw err;
    } finally {
        if (lock) await lock.release().catch(() => {});
    }
  }

  async _executeStateCommit(updates) {
      return await this.db.batch(updates);
  }

  async init() {
    this.isRunning = true;
    logger.info({ node: this.nodeId }, "MEDOR TRANSACTION ENGINE: ONLINE");
  }

  _setupGracefulShutdown() {
      const shutdown = async (s) => {
          this.isRunning = false;
          await this.redis.quit().catch(() => {});
          process.exit(0);
      };
      process.on('SIGINT', () => shutdown('SIGINT'));
      process.on('SIGTERM', () => shutdown('SIGTERM'));
  }
}

module.exports = TransactionEngine;
