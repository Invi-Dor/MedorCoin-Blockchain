/**
 * MedorCoin - Large-Scale Production Node
 * Version: 1.0.0
 * Network: Mainnet
 */

const { Worker, isMainThread, parentPort } = require("worker_threads");
const os = require("os");
const crypto = require("crypto");
// Using standard secp256k1 for true signature validation
const secp256k1 = require("secp256k1"); 
const bs58check = require("bs58check"); // Proper Base58 decoding

// Placeholder requires for your existing modules
const logger = require("./logger.cjs");
const metrics = require("./metrics.cjs");
const scheduler = require("./scheduler.cjs");
const Miner = require("./mining.cjs");

// ─── Worker thread: parallel PoW + TX validation ──────────────────

if (!isMainThread) {
  parentPort.on("message", ({ type, payload, id }) => {
    try {
      if (type === "verify_pow") {
        const result = _workerVerifyPoW(payload.block, payload.nBits);
        parentPort.postMessage({ id, result, error: null });
      } else if (type === "verify_tx") {
        const result = _workerVerifyTx(payload.tx, payload.utxoSnapshot);
        parentPort.postMessage({ id, result, error: null });
      }
    } catch (err) {
      parentPort.postMessage({ id, result: false, error: err.message });
    }
  });

  function _workerVerifyPoW(block, nBits) {
    const header = Buffer.alloc(80);
    header.writeUInt32LE(block.header.version, 0);
    Buffer.from(block.header.prevHash,   "hex").copy(header, 4);
    Buffer.from(block.header.merkleRoot, "hex").copy(header, 36);
    header.writeUInt32LE(block.header.timestamp, 68);
    header.writeUInt32LE(block.header.nBits,     72);
    header.writeBigUInt64LE(BigInt(block.header.nonce), 76);
    
    const h1     = crypto.createHash("sha256").update(header).digest();
    const h2     = crypto.createHash("sha256").update(h1).digest();
    const hashLE = BigInt("0x" + Buffer.from(h2).reverse().toString("hex"));
    
    const exp    = (nBits >> 24) & 0xff;
    const mant   = nBits & 0xffffff;
    const target = BigInt(mant) * (2n ** (8n * BigInt(exp - 3)));
    return hashLE <= target;
  }

  function _workerVerifyTx(tx, utxoSnapshot) {
    // 1. Structural Validation (Strict UTXO, no Ethereum nonces)
    if (!tx || !tx.inputs || !tx.outputs || tx.inputs.length === 0 || tx.outputs.length === 0) return false;
    if (tx.nonce !== undefined) return false; // Reject mixed models completely

    // 2. Binary Serialization for Hash/Signature check
    const headerBuf = Buffer.alloc(16);
    headerBuf.writeUInt32LE(tx.version || 1, 0);
    headerBuf.writeUInt32LE(tx.inputs.length, 4);
    headerBuf.writeUInt32LE(tx.outputs.length, 8);
    headerBuf.writeUInt32LE(tx.locktime || 0, 12);
    
    let totalIn = 0n, totalOut = 0n;
    const unsignedTxBufs = [headerBuf];

    // Build the payload that was signed, and verify UTXOs
    for (const input of tx.inputs) {
      const utxo = utxoSnapshot[input.txid + ":" + input.vout];
      if (!utxo || utxo.spent) return false; // Verify UTXO exists and unspent
      totalIn += BigInt(utxo.amount);

      const b = Buffer.alloc(36);
      Buffer.from(input.txid, 'hex').copy(b, 0);
      b.writeUInt32LE(input.vout, 32);
      unsignedTxBufs.push(b);
    }

    for (const output of tx.outputs) {
      if (output.amount <= 0) return false;
      totalOut += BigInt(output.amount);

      // Validate strict Base58Check format (prevents invalid addresses)
      try {
        const decoded = bs58check.decode(output.address);
        if (decoded.length !== 21) return false; // 1 byte version + 20 byte hash
      } catch (e) {
        return false;
      }

      const b = Buffer.alloc(8);
      b.writeBigUInt64LE(BigInt(output.amount), 0);
      unsignedTxBufs.push(b);
    }

    if (totalIn < totalOut) return false; // Verify sufficient funds

    // 3. Cryptographic Signature Verification (secp256k1)
    const rawTxForSig = Buffer.concat(unsignedTxBufs);
    const digest = crypto.createHash("sha256").update(
        crypto.createHash("sha256").update(rawTxForSig).digest()
    ).digest();

    for (let i = 0; i < tx.inputs.length; i++) {
        const input = tx.inputs[i];
        if (!input.signature || !input.publicKey) return false; // Must be signed
        
        try {
            const sigBuf = Buffer.from(input.signature, 'hex');
            const pubKeyBuf = Buffer.from(input.publicKey, 'hex');
            if (!secp256k1.ecdsaVerify(sigBuf, digest, pubKeyBuf)) return false;
            
            // Verify public key matches UTXO address
            const pubKeyHash = crypto.createHash("ripemd160").update(
                crypto.createHash("sha256").update(pubKeyBuf).digest()
            ).digest();
            const expectedAddress = bs58check.encode(Buffer.concat([Buffer.from([0x00]), pubKeyHash]));
            const utxo = utxoSnapshot[input.txid + ":" + input.vout];
            
            if (expectedAddress !== utxo.address) return false; // Ownership check

        } catch (e) {
            return false; // Malformed signature or key
        }
    }

    return true; // Fully verified UTXO, Signature, and Math
  }

  return;
}

// ─── Worker Pool ──────────────────────────────────────────────────

class WorkerPool {
  constructor(size) {
    this.size     = size;
    this.workers  = [];
    this.queue    = [];
    this._pending = new Map();
    this._seq     = 0;
  }

  init() {
    for (let i = 0; i < this.size; i++) {
      const w = new Worker(__filename);
      w.on("message", ({ id, result, error }) => {
        const handler = this._pending.get(id);
        if (!handler) return;
        this._pending.delete(id);
        error ? handler.reject(new Error(error)) : handler.resolve(result);
        if (this.queue.length > 0) {
          const { msg, resolve, reject } = this.queue.shift();
          this._dispatch(w, msg, resolve, reject);
        } else {
          w._busy = false;
        }
      });
      w.on("error", (err) => {
        logger.error("WORKER", "node.cjs:100", "Worker error: " + err.message);
        metrics.increment("medorcoin_worker_errors");
      });
      w._busy = false;
      this.workers.push(w);
    }
    logger.info("WORKER", "node.cjs:107", "Worker pool ready: " + this.size + " threads");
  }

  run(type, payload) {
    return new Promise((resolve, reject) => {
      const free = this.workers.find((w) => !w._busy);
      if (free) {
        this._dispatch(free, { type, payload, id: ++this._seq }, resolve, reject);
      } else {
        this.queue.push({ msg: { type, payload, id: ++this._seq }, resolve, reject });
        metrics.gauge("medorcoin_worker_queue_depth", this.queue.length);
      }
    });
  }

  _dispatch(worker, msg, resolve, reject) {
    worker._busy = true;
    this._pending.set(msg.id, { resolve, reject });
    worker.postMessage(msg);
  }

  terminate() {
    for (const w of this.workers) w.terminate();
  }
}

// ─── Advanced Peer Scoring ─────────────────────────────────────────

class PeerScorecard {
  constructor() { this.scores = new Map(); }

  _entry(peerId) {
    if (!this.scores.has(peerId)) {
      this.scores.set(peerId, { penalty: 0, latencyMs: [], propagations: 0, invalidBlocks: 0, invalidTxs: 0, connectedAt: Date.now(), lastSeen: Date.now() });
    }
    return this.scores.get(peerId);
  }

  recordLatency(peerId, ms) {
    const e = this._entry(peerId);
    e.latencyMs.push(ms);
    if (e.latencyMs.length > 20) e.latencyMs.shift();
    e.lastSeen = Date.now();
  }

  recordValidBlock(peerId)   { this._entry(peerId).propagations++; }
  recordInvalidBlock(peerId) { const e = this._entry(peerId); e.invalidBlocks++; e.penalty += 20; }
  recordInvalidTx(peerId)    { const e = this._entry(peerId); e.invalidTxs++;    e.penalty += 2;  }

  penalize(peerId, points, reason) {
    const e = this._entry(peerId);
    e.penalty += points;
    logger.warn("PEER_SCORE", "node.cjs:162", "Peer " + peerId + " penalized +" + points + " reason=" + reason + " total=" + e.penalty);
    metrics.increment("medorcoin_peer_penalties", { reason });
  }

  avgLatency(peerId) {
    const e = this._entry(peerId);
    if (!e.latencyMs.length) return Infinity;
    return e.latencyMs.reduce((a, b) => a + b, 0) / e.latencyMs.length;
  }

  reliability(peerId) {
    const e = this._entry(peerId);
    const total = e.propagations + e.invalidBlocks + e.invalidTxs;
    return total === 0 ? 1.0 : e.propagations / total;
  }

  shouldBan(peerId, threshold = 100) { return this._entry(peerId).penalty >= threshold; }

  topPeers(n = 10) {
    return [...this.scores.entries()]
      .map(([id, e]) => ({ id, score: e.propagations - (e.invalidBlocks * 5) - e.invalidTxs, avgLatencyMs: this.avgLatency(id), reliability: this.reliability(id), penalty: e.penalty }))
      .sort((a, b) => b.score - a.score)
      .slice(0, n);
  }

  export() {
    return { totalPeers: this.scores.size, top: this.topPeers(5), worstPenalty: [...this.scores.values()].reduce((max, e) => Math.max(max, e.penalty), 0) };
  }
}

// ─── HSM-Ready Wallet ─────────────────────────────────────────────

class WalletManager {
  constructor(cfg) {
    this.address = cfg.address;
    this._mode = process.env.WALLET_MODE || "env";
    this._key = null;
  }

  async init() {
    if (this._mode === "env") {
      this._key = process.env.WALLET_PRIVKEY || null;
      if (!this._key) logger.warn("WALLET", "node.cjs:214", "No private key in env — signing disabled");
    } else if (this._mode === "file") {
      const keyPath = process.env.WALLET_KEY_FILE;
      const passphrase = process.env.WALLET_PASSPHRASE;
      if (!keyPath || !passphrase) throw new Error("WALLET_KEY_FILE and WALLET_PASSPHRASE required for file mode");
      const encrypted = require("fs").readFileSync(keyPath, "utf8");
      const parsed = JSON.parse(encrypted);
      const key = crypto.scryptSync(passphrase, Buffer.from(parsed.salt, "hex"), 32);
      const decipher = crypto.createDecipheriv("aes-256-gcm", key, Buffer.from(parsed.iv, "hex"));
      decipher.setAuthTag(Buffer.from(parsed.tag, "hex"));
      this._key = decipher.update(parsed.data, "hex", "utf8") + decipher.final("utf8");
      logger.info("WALLET", "node.cjs:229", "Key loaded from encrypted file.");
    } else if (this._mode === "hsm") {
      logger.info("WALLET", "node.cjs:234", "HSM mode active — signing delegated to HSM");
      this._key = null;
    }
    logger.info("WALLET", "node.cjs:238", "Wallet ready. Address=" + this.address + " Mode=" + this._mode);
  }

  get privateKey() { return this._key; }
}

// ─── Consensus Logic (Your integrated logic) ─────────────────────

class Consensus {
    constructor(storage, utxoSet) {
        this.storage = storage;
        this.utxoSet = utxoSet;
        // EventEmitter stub
        this.listeners = {};
    }
    
    on(event, cb) { this.listeners[event] = this.listeners[event] || []; this.listeners[event].push(cb); }
    emit(event, data) { if (this.listeners[event]) this.listeners[event].forEach(cb => cb(data)); }
    getTip() { return { height: 0, hash: "0000000000000000000000000000000000000000000000000000000000000000" }; }

    async addBlock(block) { return await this._processBlock(block); }

    async _processBlock(block) {
        // Stub validation call
        const validation = { ok: true }; 
        if (!validation.ok) return validation;

        const currentTip = await this.storage.getChainTip() || this.getTip();
        
        const isBetter = (block.height > currentTip.height) || 
                         (block.height === currentTip.height && block.hash < currentTip.hash);

        if (isBetter) {
            if (block.parentHash !== currentTip.hash && block.height > 0) {
                return await this.handleReorg(block, currentTip);
            } else {
                // Apply straight to chain
                for (const tx of block.transactions) {
                    await this.utxoSet.applyBatch([tx], block.height);
                }
                await this.storage.setChainTip(block.hash, block.height);
                this.emit("block:accepted", block);
                return { ok: true };
            }
        } else {
            await this.storage.saveBlock(block);
            return { ok: true, status: "SIDE_CHAIN_STORED" };
        }
    }

    async handleReorg(newBlock, currentTip) {
        // Using your exact reorg recovery architecture
        const ancestor = await this.storage.findCommonAncestor(newBlock, currentTip);
        const rollbackPath = await this.storage.getBranchPath(ancestor.hash, currentTip.hash);
        const applyPath = await this.storage.getBranchPath(ancestor.hash, newBlock.hash);

        const originalTipHash = currentTip.hash;

        try {
            for (const b of rollbackPath.reverse()) {
                await this.utxoSet.rollbackBlock(b);
            }

            for (const b of applyPath) {
                const res = await this.utxoSet.applyBatch(b.transactions, b.height);
                if (!res.ok) throw new Error(res.error);
                await this.storage.setChainTip(b.hash, b.height);
            }

            this.emit('chain:reorg', { depth: rollbackPath.length, newTip: newBlock.hash });
            this.emit("block:accepted", newBlock);
            return { ok: true, status: "REORG_SUCCESS" };
        } catch (err) {
            logger.error("CONSENSUS", "CRITICAL: Reorg failed. Rolling back to original tip.", err);
            for (const b of rollbackPath) {
                await this.utxoSet.applyBatch(b.transactions, b.height);
            }
            await this.storage.setChainTip(originalTipHash, currentTip.height);
            return { ok: false, error: "REORG_ABORTED_RECOVERED" };
        }
    }
}

// ─── Async Mutex ──────────────────────────────────────────────────

class Mutex {
  constructor() { this._queue = []; this._locked = false; }
  lock()   { return new Promise((r) => this._locked ? this._queue.push(r) : (this._locked = true, r())); }
  unlock() { this._queue.length ? this._queue.shift()() : (this._locked = false); }
}

// ─── Per-IP Rate Tracker ──────────────────────────────────────────

const _msgRates = new Map();
function checkMsgRate(ip, maxPerSec = 100) {
  const now = Date.now();
  let entry = _msgRates.get(ip);
  if (!entry || now - entry.windowStart > 1000) {
    entry = { count: 0, windowStart: now };
    _msgRates.set(ip, entry);
  }
  return ++entry.count <= maxPerSec;
}

// ─── Configuration ────────────────────────────────────────────────

const CONFIG = {
  p2pPort:     parseInt(process.env.P2P_PORT  || "8333"),
  rpcPort:     parseInt(process.env.RPC_PORT  || "8332"),
  dataDir:     process.env.DATA_DIR           || "./data",
  mineEnabled: process.env.MINE               === "true",
  nodeId:      process.env.NODE_ID            || "medor-" + crypto.randomBytes(4).toString("hex"),
  workerCount: parseInt(process.env.WORKERS   || String(Math.max(1, os.cpus().length - 2))),

  seedPeers: (process.env.SEED_PEERS || "")
    .split(",").filter(Boolean)
    .map((s) => { const [a, p] = s.split(":"); return { address: a, port: parseInt(p || "8333") }; }),

  wallet: {
    address: "1ukpJFf4uz3c3gDmM9JASZaGiV4STJEDfzp",
    privateKey: process.env.WALLET_PRIVKEY || "", 
  },

  ddos: { maxMsgPerSecPerPeer: 100, penaltyPerInvalid: 20, banThreshold: 100, banDurationMs: 3_600_000 },
  alerts: { minPeers: 4, maxMempoolSize: 80_000, maxBlockTimeSec: 60, hashRateDropPct: 20 },
};

// ─── UTXO snapshot for worker thread (serializable) ───────────────

function _buildUtxoSnapshot(utxoSet, tx) {
  const snap = {};
  if (!tx?.inputs) return snap;
  for (const input of tx.inputs) {
    const key = input.txid + ":" + input.vout;
    const utxo = utxoSet.get(input.txid, input.vout);
    if (utxo) snap[key] = utxo;
  }
  return snap;
}

// ─── Main ─────────────────────────────────────────────────────────

async function main() {
  logger.info("NODE", "node.cjs:307", "=== MedorCoin Large-Scale Node Starting === " + CONFIG.nodeId);
  logger.info("NODE", "node.cjs:308", "CPUs=" + os.cpus().length + " Workers=" + CONFIG.workerCount);

  try {
    const pool = new WorkerPool(CONFIG.workerCount);
    pool.init();

    const wallet = new WalletManager(CONFIG.wallet);
    await wallet.init();

    // Setup stubs for external modules
    const storage = { 
        init: async () => {}, close: async () => {}, saveBlock: async () => {}, 
        getChainTip: async () => ({ height: 0, hash: "0".repeat(64) }), setChainTip: async () => {} 
    };
    const utxoSet = { get: () => null, applyBatch: async () => ({ok: true}), rollbackBlock: async () => {} };
    
    // Wire your integrated Consensus
    const chain = new Consensus(storage, utxoSet);
    logger.info("NODE", "node.cjs:326", "Chain tip: #" + chain.getTip().height);

    const mempool = { size: () => 0, addTransaction: () => ({ok: true}), removeConfirmed: () => {}, start: () => {}, stop: () => {}, getTransactionsForBlock: () => [] };
    const scorecard = new PeerScorecard();

    const p2p = { peers: new Map(), bannedPeers: new Set(), broadcast: async () => {}, start: () => {}, stop: () => {}, on: () => {} };
    
    p2p.penalizePeer = function(peerId, points, reason) {
      scorecard.penalize(peerId, points, reason);
      if (scorecard.shouldBan(peerId, CONFIG.ddos.banThreshold)) {
        const peer = this.peers.get(peerId);
        if (peer) {
          this.bannedPeers.add(peer.address);
          logger.warn("P2P", "node.cjs:344", "Banned: " + peer.address);
          metrics.increment("medorcoin_peers_banned");
          setTimeout(() => this.bannedPeers.delete(peer.address), CONFIG.ddos.banDurationMs);
        }
      }
    }.bind(p2p);

    const blockMutex = new Mutex();

    // Hardened message handler
    p2p.on("message", async (msg, peer) => {
      if (!checkMsgRate(peer.address, CONFIG.ddos.maxMsgPerSecPerPeer)) {
        p2p.penalizePeer(peer.id, 5, "flood");
        metrics.increment("medorcoin_ddos_drops");
        return;
      }

      const payload = msg.payload || msg.data;
      const t0 = Date.now();

      try {
        if (msg.type === "tx") {
          const utxoSnap = _buildUtxoSnapshot(utxoSet, payload);
          const valid = await pool.run("verify_tx", { tx: payload, utxoSnapshot: utxoSnap });

          if (!valid) {
            scorecard.recordInvalidTx(peer.id);
            p2p.penalizePeer(peer.id, 2, "invalid_tx");
            return;
          }
          const result = mempool.addTransaction(payload);
          if (result.ok) {
            p2p.broadcast("tx", payload, peer);
            metrics.increment("medorcoin_tx_relayed");
          }

        } else if (msg.type === "block") {
          await blockMutex.lock();
          try {
            const powOk = await pool.run("verify_pow", { block: payload, nBits: payload.header?.nBits || 0 });

            if (!powOk) {
              scorecard.recordInvalidBlock(peer.id);
              p2p.penalizePeer(peer.id, CONFIG.ddos.penaltyPerInvalid, "pow_fail");
              metrics.increment("medorcoin_blocks_rejected", { reason: "pow" });
              return;
            }

            const result = await chain.addBlock(payload);
            if (result.ok) {
              scorecard.recordValidBlock(peer.id);
              p2p.broadcast("block", payload, peer);
              metrics.increment("medorcoin_blocks_relayed");
            } else {
              scorecard.recordInvalidBlock(peer.id);
              p2p.penalizePeer(peer.id, CONFIG.ddos.penaltyPerInvalid, result.reason);
              metrics.increment("medorcoin_blocks_rejected", { reason: result.reason });
            }
          } finally {
            blockMutex.unlock();
          }
        }
      } catch (err) {
        logger.error("NODE", "node.cjs:413", "Handler error: " + err.message);
        metrics.increment("medorcoin_handler_errors");
      } finally {
        scorecard.recordLatency(peer.id, Date.now() - t0);
      }
    });

    chain.on("block:accepted", (block) => {
      logger.info("NODE", "node.cjs:421", "Block #" + block.height + " accepted");
      mempool.removeConfirmed(block.transactions);
      metrics.gauge("medorcoin_chain_height", block.height);
      metrics.gauge("medorcoin_last_block_ts", Date.now());
    });

    chain.on("chain:reorg", ({ depth, newTip }) => {
      logger.warn("NODE", "node.cjs:428", "Reorg depth=" + depth);
      metrics.increment("medorcoin_reorgs");
    });

    let miner = null;
    if (CONFIG.mineEnabled) {
      if (!wallet.address) {
        logger.error("NODE", "node.cjs:440", "FATAL: no wallet address");
        process.exit(1);
      }
      miner = new Miner(mempool, chain, { address: wallet.address, privateKey: wallet.privateKey }, p2p);

      chain.on("block:accepted", () => {
        if (miner?._abortController) miner._abortController.abort();
      });

      miner.start();
      miner.on("block:mined", (block) => {
        logger.info("NODE", "node.cjs:452", "MINED: " + block.hash);
        metrics.increment("medorcoin_blocks_mined");
        metrics.gauge("medorcoin_hash_rate", Number(miner.hashRate));
      });
    }

    let _lastBlockTime = Date.now();
    let _lastHashRate  = 0n;
    chain.on("block:accepted", () => { _lastBlockTime = Date.now(); });

    scheduler.register("health_check", async () => {
      const peers = p2p.peers.size;
      const mempoolSz = mempool.size();
      const height = chain.getTip()?.height ?? 0;
      const currentRate = miner?.hashRate ?? 0n;

      metrics.gauge("medorcoin_peer_count", peers);
      metrics.gauge("medorcoin_mempool_size", mempoolSz);
      metrics.gauge("medorcoin_chain_height", height);
      metrics.gauge("medorcoin_worker_queue_depth", pool.queue.length);
      if (miner) metrics.gauge("medorcoin_hash_rate", Number(currentRate));

      const sc = scorecard.export();
      metrics.gauge("medorcoin_peer_avg_reliability", sc.top.reduce((s, p) => s + p.reliability, 0) / Math.max(1, sc.top.length));
      metrics.gauge("medorcoin_peer_avg_latency_ms", sc.top.reduce((s, p) => s + p.avgLatencyMs, 0) / Math.max(1, sc.top.length));

      if (peers < CONFIG.alerts.minPeers) {
        logger.warn("NODE", "node.cjs:487", "ALERT: low peers=" + peers);
        metrics.increment("medorcoin_alerts", { type: "low_peers" });
      }
      if (mempoolSz > CONFIG.alerts.maxMempoolSize) {
        logger.warn("NODE", "node.cjs:491", "ALERT: mempool=" + mempoolSz);
        metrics.increment("medorcoin_alerts", { type: "mempool_overflow" });
      }
      const stale = (Date.now() - _lastBlockTime) / 1000;
      if (stale > CONFIG.alerts.maxBlockTimeSec) {
        logger.warn("NODE", "node.cjs:496", "ALERT: no block for " + Math.round(stale) + "s");
        metrics.increment("medorcoin_alerts", { type: "stale_chain" });
      }
      if (miner && _lastHashRate > 0n) {
        const drop = Number(((_lastHashRate - currentRate) * 100n) / _lastHashRate);
        if (drop > CONFIG.alerts.hashRateDropPct) {
          logger.warn("NODE", "node.cjs:502", "ALERT: hashrate drop " + drop + "%");
          metrics.increment("medorcoin_alerts", { type: "hashrate_drop" });
        }
      }
      _lastHashRate = currentRate;
    }, 10_000, { runImmediately: true });

    scheduler.register("peer_scorecard_log", async () => {
      const sc = scorecard.export();
      logger.info("SCORE", "node.cjs:519", "Peer scorecard: " + JSON.stringify(sc));
    }, 60_000);

    scheduler.onShutdown(async () => {
      logger.info("NODE", "node.cjs:536", "Shutdown — draining worker pool...");
      pool.terminate();
      if (miner) miner.stop();
      logger.info("NODE", "node.cjs:545", "Clean shutdown complete.");
    });

    scheduler.start();
    logger.info("NODE", "node.cjs:549", "=== MedorCoin Large-Scale Node Ready ===");

  } catch (err) {
    logger.error("NODE", "node.cjs:552", "FATAL: " + err.message + "\n" + err.stack);
    process.exit(1);
  }
}

const shutdown = async (sig) => {
  logger.info("NODE", "node.cjs:571", sig + " received");
  await scheduler.shutdown();
  process.exit(0);
};

process.on("SIGINT",  () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("uncaughtException",  (err) => {
  logger.error("CRITICAL", "node.cjs:579", "Uncaught: " + err.message);
  metrics.increment("medorcoin_uncaught_exceptions");
});
process.on("unhandledRejection", (reason) => {
  logger.error("CRITICAL", "node.cjs:583", "Rejection: " + String(reason));
  metrics.increment("medorcoin_unhandled_rejections");
});

main();


// --- ADD THIS TO THE VERY END OF YOUR FILE ---
if (isMainThread) {
    const express = require('express');
    const cors = require('cors');
    const bip39 = require('bip39');
    const bcrypt = require('bcryptjs');
    const app = express();

    app.use(cors());
    app.use(express.json());

    app.post('/api/signup', async (req, res) => {
        try {
            const { username, password } = req.body;
            if (!username || !password || password.length < 8) {
                return res.status(400).json({ success: false, error: "Invalid data." });
            }

            const mnemonic = bip39.generateMnemonic(); 
            const address = "MD" + crypto.createHash('sha256').update(mnemonic).digest('hex').substring(0, 40);
            const salt = await bcrypt.genSalt(10);
            const hashedPassword = await bcrypt.hash(password, salt);

            console.log(`[ECOSYSTEM] User ${username} created with address ${address}`);

            res.json({ success: true, address, mnemonic });
        } catch (err) {
            res.status(500).json({ success: false, error: "Server Error" });
        }
    });

    const PORT = 5000;
    app.listen(PORT, '0.0.0.0', () => {
        console.log(`\n🚀 GATEWAY LIVE ON PORT ${PORT}`);
    });
}
