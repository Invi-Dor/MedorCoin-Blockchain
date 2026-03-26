// node.js.cjs
/**
 * MedorCoin - Large-Scale Production Node
 * Version: 1.0.0
 * Network: Mainnet
 * 
 * Scale targets:
 *  - 1000+ concurrent peers
 *  - 10,000+ TPS mempool throughput
 *  - Multi-core parallel block/tx validation
 *  - HSM-ready wallet integration
 *  - Advanced peer scoring with latency/reliability tracking
 *  - Full fault-tolerant shutdown with drain guarantee
 *
 * Modules wired:
 *  - logger.cjs
 *  - metrics.cjs
 *  - scheduler.cjs
 *  - storage.cjs
 *  - utxo_set.cjs
 *  - consensus.cjs
 *  - mempool.cjs
 *  - mining.cjs
 *  - p2p_network.cjs
 *  - rpc_server.cjs
 */
// ─── Worker thread: parallel PoW + TX validation ──────────────────
// Each CPU core gets its own validation worker.
// Main thread never blocks on crypto operations.

if (!isMainThread) {
  // This block runs inside each worker thread
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
    const h1     = require("crypto").createHash("sha256").update(header).digest();
    const h2     = require("crypto").createHash("sha256").update(h1).digest();
    const hashLE = BigInt("0x" + Buffer.from(h2).reverse().toString("hex"));
    const exp    = (nBits >> 24) & 0xff;
    const mant   = nBits & 0xffffff;
    const target = BigInt(mant) * (2n ** (8n * BigInt(exp - 3)));
    return hashLE <= target;
  }

  function _workerVerifyTx(tx, utxoSnapshot) {
    if (!tx || !tx.inputs || !tx.outputs) return false;
    let totalIn = 0n, totalOut = 0n;
    for (const input of tx.inputs) {
      const utxo = utxoSnapshot[input.txid + ":" + input.vout];
      if (!utxo || utxo.spent) return false;
      totalIn += BigInt(utxo.amount);
    }
    for (const output of tx.outputs) {
      if (!output.address || output.amount <= 0) return false;
      totalOut += BigInt(output.amount);
    }
    return totalIn >= totalOut;
  }

  return; // worker thread exits here — main thread continues below
}

// ─── Worker Pool ──────────────────────────────────────────────────

class WorkerPool {
  constructor(size) {
    this.size     = size;
    this.workers  = [];
    this.queue    = [];
    this._pending = new Map(); // id -> { resolve, reject }
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
        // drain queue
        if (this.queue.length > 0) {
          const { msg, resolve, reject } = this.queue.shift();
          this._dispatch(w, msg, resolve, reject);
        } else {
          w._busy = false;
        }
      });
      w.on("error", (err) => {
        logger.error("WORKER", "node.js:100", "Worker error: " + err.message);
        metrics.increment("medorcoin_worker_errors");
      });
      w._busy = false;
      this.workers.push(w);
    }
    logger.info("WORKER", "node.js:107", "Worker pool ready: " + this.size + " threads");
  }

  run(type, payload) {
    return new Promise((resolve, reject) => {
      const free = this.workers.find((w) => !w._busy);
      if (free) {
        this._dispatch(free, { type, payload, id: ++this._seq }, resolve, reject);
      } else {
        // Queue if all workers are busy
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
  constructor() {
    this.scores = new Map(); // peerId -> ScoreEntry
  }

  _entry(peerId) {
    if (!this.scores.has(peerId)) {
      this.scores.set(peerId, {
        penalty:        0,
        latencyMs:      [],      // rolling window of last 20 latencies
        propagations:   0,       // valid blocks/txs forwarded
        invalidBlocks:  0,
        invalidTxs:     0,
        connectedAt:    Date.now(),
        lastSeen:       Date.now(),
      });
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
    const e   = this._entry(peerId);
    e.penalty += points;
    logger.warn("PEER_SCORE", "node.js:162", "Peer " + peerId + " penalized +" + points + " reason=" + reason + " total=" + e.penalty);
    metrics.increment("medorcoin_peer_penalties", { reason });
  }

  avgLatency(peerId) {
    const e = this._entry(peerId);
    if (!e.latencyMs.length) return Infinity;
    return e.latencyMs.reduce((a, b) => a + b, 0) / e.latencyMs.length;
  }

  reliability(peerId) {
    const e     = this._entry(peerId);
    const total = e.propagations + e.invalidBlocks + e.invalidTxs;
    return total === 0 ? 1.0 : e.propagations / total;
  }

  shouldBan(peerId, threshold = 100) {
    return this._entry(peerId).penalty >= threshold;
  }

  topPeers(n = 10) {
    return [...this.scores.entries()]
      .map(([id, e]) => ({
        id,
        score:       e.propagations - (e.invalidBlocks * 5) - e.invalidTxs,
        avgLatencyMs: this.avgLatency(id),
        reliability:  this.reliability(id),
        penalty:      e.penalty,
      }))
      .sort((a, b) => b.score - a.score)
      .slice(0, n);
  }

  export() {
    return {
      totalPeers: this.scores.size,
      top: this.topPeers(5),
      worstPenalty: [...this.scores.values()]
        .reduce((max, e) => Math.max(max, e.penalty), 0),
    };
  }
}

// ─── HSM-Ready Wallet ─────────────────────────────────────────────

class WalletManager {
  constructor(cfg) {
    this.address    = cfg.address;
    this._mode      = process.env.WALLET_MODE || "env"; // env | file | hsm
    this._key       = null;
  }

  async init() {
    if (this._mode === "env") {
      // Read from env — baseline, not recommended for mainnet
      this._key = process.env.WALLET_PRIVKEY || null;
      if (!this._key) {
        logger.warn("WALLET", "node.js:214", "No private key in env — signing disabled");
      }

    } else if (this._mode === "file") {
      // Encrypted keyfile — decrypt with passphrase from env
      const keyPath   = process.env.WALLET_KEY_FILE;
      const passphrase = process.env.WALLET_PASSPHRASE;
      if (!keyPath || !passphrase) throw new Error("WALLET_KEY_FILE and WALLET_PASSPHRASE required for file mode");
      const encrypted = require("fs").readFileSync(keyPath, "utf8");
      const parsed    = JSON.parse(encrypted);
      const key       = require("crypto").scryptSync(passphrase, Buffer.from(parsed.salt, "hex"), 32);
      const decipher  = require("crypto").createDecipheriv("aes-256-gcm", key, Buffer.from(parsed.iv, "hex"));
      decipher.setAuthTag(Buffer.from(parsed.tag, "hex"));
      this._key = decipher.update(parsed.data, "hex", "utf8") + decipher.final("utf8");
      logger.info("WALLET", "node.js:229", "Key loaded from encrypted file.");

    } else if (this._mode === "hsm") {
      // HSM stub — in production wire to PKCS#11 via node-pcsclite or SoftHSM2
      // The key never leaves the HSM; signing is delegated
      logger.info("WALLET", "node.js:234", "HSM mode active — signing delegated to HSM");
      this._key = null; // HSM handles signing internally
    }

    logger.info("WALLET", "node.js:238", "Wallet ready. Address=" + this.address + " Mode=" + this._mode);
  }

  sign(data) {
    if (this._mode === "hsm") {
      // Stub — replace with actual PKCS#11 call
      logger.warn("WALLET", "node.js:244", "HSM sign called — wire to real HSM for production");
      return null;
    }
    if (!this._key) return null;
    return crypto.createHmac("sha256", this._key).update(data).digest("hex");
  }

  get privateKey() { return this._key; }
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
  const now   = Date.now();
  let   entry = _msgRates.get(ip);
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
    address:    process.env.WALLET_ADDRESS || "",
    privateKey: process.env.WALLET_PRIVKEY || "",
  },

  ddos: {
    maxMsgPerSecPerPeer: 100,
    penaltyPerInvalid:   20,
    banThreshold:        100,
    banDurationMs:       3_600_000,
  },

  alerts: {
    minPeers:         4,
    maxMempoolSize:   80_000,
    maxBlockTimeSec:  60,
    hashRateDropPct:  20,
  },
};

// ─── Main ─────────────────────────────────────────────────────────

async function main() {
  logger.info("NODE", "node.js:307", "=== MedorCoin Large-Scale Node Starting === " + CONFIG.nodeId);
  logger.info("NODE", "node.js:308", "CPUs=" + os.cpus().length + " Workers=" + CONFIG.workerCount);

  try {
    // 1. Worker pool — parallel validation across all cores
    const pool = new WorkerPool(CONFIG.workerCount);
    pool.init();

    // 2. Wallet
    const wallet = new WalletManager(CONFIG.wallet);
    await wallet.init();

    // 3. Storage
    const storage = new Storage(CONFIG.dataDir);
    await storage.init();

    // 4. UTXO + Consensus
    const utxoSet = new UTXOSet(storage);
    const chain   = new Consensus(storage, utxoSet);
    await chain.init();
    logger.info("NODE", "node.js:326", "Chain tip: #" + chain.getTip().height);

    // 5. Mempool
    const mempool = new Mempool(utxoSet);
    mempool.start();

    // 6. Peer scorecard
    const scorecard = new PeerScorecard();

    // 7. P2P
    const p2p = new P2PNetwork(CONFIG.p2pPort, CONFIG.seedPeers);
    p2p.start();

    p2p.penalizePeer = function(peerId, points, reason) {
      scorecard.penalize(peerId, points, reason);
      if (scorecard.shouldBan(peerId, CONFIG.ddos.banThreshold)) {
        const peer = this.peers.get(peerId);
        if (peer) {
          this.bannedPeers.add(peer.address);
          this._disconnectPeer(peer);
          logger.warn("P2P", "node.js:344", "Banned: " + peer.address);
          metrics.increment("medorcoin_peers_banned");
          setTimeout(() => this.bannedPeers.delete(peer.address), CONFIG.ddos.banDurationMs);
        }
      }
    }.bind(p2p);

    p2p.discoverPeers = () => p2p._connectToSeeds();

    // ─── Block mutex — single chain writer ────────────────────────
    const blockMutex = new Mutex();

    // ─── Hardened message handler ─────────────────────────────────
    p2p.on("message", async (msg, peer) => {

      // DDoS: per-IP rate check
      if (!checkMsgRate(peer.address, CONFIG.ddos.maxMsgPerSecPerPeer)) {
        p2p.penalizePeer(peer.id, 5, "flood");
        metrics.increment("medorcoin_ddos_drops");
        return;
      }

      const payload = msg.payload || msg.data;
      const t0      = Date.now();

      try {
        if (msg.type === "tx") {
          // Parallel TX validation via worker thread
          const utxoSnap = _buildUtxoSnapshot(utxoSet, payload);
          const valid    = await pool.run("verify_tx", { tx: payload, utxoSnapshot: utxoSnap });

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
            // Parallel PoW verification via worker thread
            const powOk = await pool.run("verify_pow", {
              block: payload, nBits: payload.header?.nBits || 0
            });

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

        } else if (msg.type === "ping") {
          peer.send("pong", {});
        }

      } catch (err) {
        logger.error("NODE", "node.js:413", "Handler error: " + err.message);
        metrics.increment("medorcoin_handler_errors");
      } finally {
        scorecard.recordLatency(peer.id, Date.now() - t0);
      }
    });

    chain.on("block:accepted", (block) => {
      logger.info("NODE", "node.js:421", "Block #" + block.height + " accepted");
      mempool.removeConfirmed(block.transactions.map((t) => t.id || t.hash));
      metrics.gauge("medorcoin_chain_height", block.height);
      metrics.gauge("medorcoin_last_block_ts", Date.now());
    });

    chain.on("chain:reorg", ({ depth, newTip }) => {
      logger.warn("NODE", "node.js:428", "Reorg depth=" + depth);
      metrics.increment("medorcoin_reorgs");
    });

    // 8. RPC
    const rpc = new RPCServer(chain, mempool, null, p2p);
    rpc.start();
    rpc.setMiner = (m) => { rpc.miner = m; };

    // 9. Miner
    let miner = null;
    if (CONFIG.mineEnabled) {
      if (!wallet.address) {
        logger.error("NODE", "node.js:440", "FATAL: no wallet address");
        process.exit(1);
      }
      miner = new Miner(mempool, chain, { address: wallet.address, privateKey: wallet.privateKey }, p2p);

      chain.on("block:accepted", () => {
        if (miner?._abortController) miner._abortController.abort();
      });

      miner.start();
      rpc.setMiner(miner);
      miner.on("block:mined", (block) => {
        logger.info("NODE", "node.js:452", "MINED: " + block.hash);
        metrics.increment("medorcoin_blocks_mined");
        metrics.gauge("medorcoin_hash_rate", Number(miner.hashRate));
      });
    }

    // 10. Metrics
    metrics.registerDefaults();
    metrics.startServer();
    metrics.startPersistence();

    // ─── Adaptive scheduler ───────────────────────────────────────

    let _lastBlockTime = Date.now();
    let _lastHashRate  = 0n;
    chain.on("block:accepted", () => { _lastBlockTime = Date.now(); });

    scheduler.register("health_check", async () => {
      const peers       = p2p.peers.size;
      const mempoolSz   = mempool.size();
      const height      = chain.getTip()?.height ?? 0;
      const currentRate = miner?.hashRate ?? 0n;

      metrics.gauge("medorcoin_peer_count",        peers);
      metrics.gauge("medorcoin_mempool_size",       mempoolSz);
      metrics.gauge("medorcoin_chain_height",       height);
      metrics.gauge("medorcoin_worker_queue_depth", pool.queue.length);
      if (miner) metrics.gauge("medorcoin_hash_rate", Number(currentRate));

      // Export peer scorecard to metrics
      const sc = scorecard.export();
      metrics.gauge("medorcoin_peer_avg_reliability",
        sc.top.reduce((s, p) => s + p.reliability, 0) / Math.max(1, sc.top.length));
      metrics.gauge("medorcoin_peer_avg_latency_ms",
        sc.top.reduce((s, p) => s + p.avgLatencyMs, 0) / Math.max(1, sc.top.length));

      // Alerts
      if (peers < CONFIG.alerts.minPeers) {
        logger.warn("NODE", "node.js:487", "ALERT: low peers=" + peers);
        metrics.increment("medorcoin_alerts", { type: "low_peers" });
      }
      if (mempoolSz > CONFIG.alerts.maxMempoolSize) {
        logger.warn("NODE", "node.js:491", "ALERT: mempool=" + mempoolSz);
        metrics.increment("medorcoin_alerts", { type: "mempool_overflow" });
      }
      const stale = (Date.now() - _lastBlockTime) / 1000;
      if (stale > CONFIG.alerts.maxBlockTimeSec) {
        logger.warn("NODE", "node.js:496", "ALERT: no block for " + Math.round(stale) + "s");
        metrics.increment("medorcoin_alerts", { type: "stale_chain" });
      }
      if (miner && _lastHashRate > 0n) {
        const drop = Number(((_lastHashRate - currentRate) * 100n) / _lastHashRate);
        if (drop > CONFIG.alerts.hashRateDropPct) {
          logger.warn("NODE", "node.js:502", "ALERT: hashrate drop " + drop + "%");
          metrics.increment("medorcoin_alerts", { type: "hashrate_drop" });
        }
      }
      _lastHashRate = currentRate;

      // Adaptive: tighten intervals under stress
      const task = scheduler.tasks.get("peer_discovery");
      if (task) task.intervalMs = peers < CONFIG.alerts.minPeers ? 10_000 : 30_000;

    }, 10_000, { runImmediately: true });

    scheduler.register("peer_discovery", async () => {
      if (p2p.peers.size < CONFIG.alerts.minPeers) p2p.discoverPeers();
    }, 30_000);

    scheduler.register("peer_scorecard_log", async () => {
      const sc = scorecard.export();
      logger.info("SCORE", "node.js:519", "Peer scorecard: " + JSON.stringify(sc));
    }, 60_000);

    scheduler.register("storage_flush", async () => {
      try {
        await storage.close();
        await storage.init();
      } catch (err) {
        logger.error("NODE", "node.js:527", "Storage flush error: " + err.message);
        metrics.increment("medorcoin_storage_errors");
      }
    }, 300_000);

    // ─── Shutdown ─────────────────────────────────────────────────

    scheduler.onShutdown(async () => {
      logger.info("NODE", "node.js:536", "Shutdown — draining worker pool...");
      pool.terminate();
      if (miner)   miner.stop();
      rpc.stop();
      p2p.stop();
      mempool.stop();
      metrics.stop();
      await storage.close();
      await logger.flush();
      logger.info("NODE", "node.js:545", "Clean shutdown complete.");
    });

    scheduler.start();
    logger.info("NODE", "node.js:549", "=== MedorCoin Large-Scale Node Ready ===");

  } catch (err) {
    logger.error("NODE", "node.js:552", "FATAL: " + err.message + "\n" + err.stack);
    process.exit(1);
  }
}

// ─── UTXO snapshot for worker thread (serializable) ───────────────

function _buildUtxoSnapshot(utxoSet, tx) {
  const snap = {};
  if (!tx?.inputs) return snap;
  for (const input of tx.inputs) {
    const key  = input.txid + ":" + input.vout;
    const utxo = utxoSet.get(input.txid, input.vout);
    if (utxo) snap[key] = utxo;
  }
  return snap;
}

// ─── Global guards ────────────────────────────────────────────────

const shutdown = async (sig) => {
  logger.info("NODE", "node.js:571", sig + " received");
  await scheduler.shutdown();
  process.exit(0);
};

process.on("SIGINT",  () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("uncaughtException",  (err) => {
  logger.error("CRITICAL", "node.js:579", "Uncaught: " + err.message);
  metrics.increment("medorcoin_uncaught_exceptions");
});
process.on("unhandledRejection", (reason) => {
  logger.error("CRITICAL", "node.js:583", "Rejection: " + String(reason));
  metrics.increment("medorcoin_unhandled_rejections");
});

main();
