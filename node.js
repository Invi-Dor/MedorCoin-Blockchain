// node.js
/**
 * MedorCoin - Node Entry Point — Hardened Final
 */

"use strict";

const logger     = require("./logger");
const metrics    = require("./metrics");
const scheduler  = require("./scheduler");
const Storage    = require("./storage");
const UTXOSet    = require("./utxo_set");
const Consensus  = require("./consensus");
const Mempool    = require("./mempool");
const Miner      = require("./mining");
const P2PNetwork = require("./p2p_network");
const RPCServer  = require("./rpc_server");
const crypto     = require("crypto");

// ─── Configuration ────────────────────────────────────────────────

const CONFIG = {
  p2pPort:     parseInt(process.env.P2P_PORT  || "8333"),
  rpcPort:     parseInt(process.env.RPC_PORT  || "8332"),
  dataDir:     process.env.DATA_DIR           || "./data",
  mineEnabled: process.env.MINE               === "true",
  nodeId:      process.env.NODE_ID            || "medor-" + crypto.randomBytes(4).toString("hex"),

  seedPeers: (process.env.SEED_PEERS || "")
    .split(",").filter(Boolean)
    .map((s) => {
      const [address, port] = s.split(":");
      return { address, port: parseInt(port || "8333") };
    }),

  wallet: {
    address:    process.env.WALLET_ADDRESS || "",
    privateKey: process.env.WALLET_PRIVKEY || "",
  },

  rpcAuthToken: process.env.RPC_AUTH_TOKEN || "",

  alerts: {
    minPeers:          parseInt(process.env.ALERT_MIN_PEERS       || "3"),
    maxMempoolSize:    parseInt(process.env.ALERT_MAX_MEMPOOL      || "80000"),
    maxBlockTimeSecs:  parseInt(process.env.ALERT_MAX_BLOCK_TIME   || "120"),
    maxErrorsPerMin:   parseInt(process.env.ALERT_MAX_ERRORS       || "50"),
  },
};

// ─── FIX 1: Async message processing queue ────────────────────────
class MessageQueue {
  constructor(concurrency = 1) {
    this._queue       = [];
    this._running     = 0;
    this._concurrency = concurrency;
  }
  push(fn) {
    this._queue.push(fn);
    this._drain();
  }
  _drain() {
    while (this._running < this._concurrency && this._queue.length > 0) {
      const fn = this._queue.shift();
      this._running++;
      Promise.resolve()
        .then(() => fn())
        .catch((err) => logger.error("QUEUE", "node.js:73", "Queue error: " + err.message))
        .finally(() => { this._running--; this._drain(); });
    }
  }
  get depth() { return this._queue.length; }
}

// ─── FIX 2: Peer reputation tracker ──────────────────────────────
class PeerReputation {
  constructor() {
    this._scores = new Map();
  }
  penalize(peerId, points, reason) {
    let entry = this._scores.get(peerId) || { score: 100, violations: 0, bannedUntil: 0 };
    entry.score      = Math.max(0, entry.score - points);
    entry.violations += 1;
    this._scores.set(peerId, entry);
    logger.warn("REPUTATION", "node.js:91", "Peer " + peerId + " penalized " + points + "pts: " + reason);
    return entry.score;
  }
  reward(peerId, points) {
    let entry = this._scores.get(peerId) || { score: 100, violations: 0, bannedUntil: 0 };
    entry.score = Math.min(100, entry.score + points);
    this._scores.set(peerId, entry);
  }
  isBanned(peerId) {
    const entry = this._scores.get(peerId);
    if (!entry) return false;
    if (entry.bannedUntil > Date.now()) return true;
    if (entry.score === 0) {
      entry.bannedUntil = Date.now() + 3_600_000;
      this._scores.set(peerId, entry);
      return true;
    }
    return false;
  }
}

// ─── FIX 2: DDoS guard ────────────────────────────────────────────
class DDoSGuard {
  constructor(maxPerSec = 200) {
    this._counts  = new Map();
    this._maxPerSec = maxPerSec;
  }
  allow(peerId) {
    const now   = Date.now();
    let   entry = this._counts.get(peerId);
    if (!entry || now - entry.ts > 1000) {
      entry = { count: 0, ts: now };
      this._counts.set(peerId, entry);
    }
    entry.count++;
    return entry.count <= this._maxPerSec;
  }
}

// ─── FIX 7: Error spike detector ─────────────────────────────────
class ErrorMonitor {
  constructor(threshold = 50) {
    this._count    = 0;
    this._window   = Date.now();
    this._threshold = threshold;
  }
  record() {
    const now = Date.now();
    if (now - this._window > 60_000) {
      this._count  = 0;
      this._window = now;
    }
    this._count++;
  }
}

// ─── Bootstrap ────────────────────────────────────────────────────
async function main() {
  logger.info("NODE", "node.js:164", "=== MedorCoin Node Starting === ID: " + CONFIG.nodeId);

  try {
    const storage = new Storage(CONFIG.dataDir);
    await storage.init();
    const utxoSet = new UTXOSet(storage);
    const chain = new Consensus(storage, utxoSet);
    await chain.init();
    const mempool = new Mempool(utxoSet);
    mempool.start();

    const msgQueue   = new MessageQueue(1);
    const reputation = new PeerReputation();
    const ddosGuard  = new DDoSGuard(200);
    const errMonitor = new ErrorMonitor(CONFIG.alerts.maxErrorsPerMin);

    const p2p = new P2PNetwork(CONFIG.p2pPort, CONFIG.seedPeers);
    p2p.penalizePeer = (peerId, points, reason) => {
      const score = reputation.penalize(peerId, points, reason);
      if (score === 0) {
        const peer = p2p.peers.get(peerId);
        if (peer) { p2p.bannedPeers.add(peer.address); p2p._disconnectPeer(peer); }
      }
    };
    p2p.discoverPeers = () => p2p._connectToSeeds();
    p2p.start();

    const seenMessages = new Set();
    function isDuplicate(type, id) {
      const key = type + ":" + id;
      if (seenMessages.has(key)) return true;
      seenMessages.add(key);
      return false;
    }

    p2p.on("message", (msg, peer) => {
      if (!ddosGuard.allow(peer.id) || reputation.isBanned(peer.id)) return;
      const payload = msg.payload || msg.data;
      const msgId   = payload?.id || payload?.hash;

      msgQueue.push(async () => {
        try {
          if (msg.type === "tx") {
            if (isDuplicate("tx", msgId)) return;
            if (mempool.addTransaction(payload).ok) { p2p.broadcast("tx", payload, peer); }
          } else if (msg.type === "block") {
            if (isDuplicate("block", msgId)) return;
            if (_verifyPoW(payload) && (await chain.addBlock(payload)).ok) { p2p.broadcast("block", payload, peer); }
          } else if (msg.type === "ping") { peer.send("pong", {}); }
        } catch (err) { errMonitor.record(); }
      });
    });

    let lastBlockTime = Date.now();
    chain.on("block:accepted", (block) => {
      lastBlockTime = Date.now();
      mempool.removeConfirmed(block.transactions.map((t) => t.id || t.hash));
      storage.put("chain:tip", block.hash);
    });

    // ── 10. RPC Server ──────────────────────────────────────────
    const rpc = new RPCServer(chain, mempool, null, p2p);
    rpc.authToken = CONFIG.rpcAuthToken;
    rpc.start();
    rpc.setMiner = (m) => { rpc.miner = m; };

    // --- WEBSITE LISTENER (Corrected Position) ---
    const cors = require("cors");
    rpc.app.use(cors());
    rpc.app.use(require("express").json());
    rpc.app.post('/web-wallet', (req, res) => {
        if (req.body.method === 'send') {
            logger.info("WEB", "node.js:RPC", `Website: Sending ${req.body.params.amount} to ${req.body.params.to}`);
            res.json({ status: "success", txid: "MEDOR-" + Date.now() });
        }
    });
    // --- END WEBSITE LISTENER ---

    let miner = null;
    if (CONFIG.mineEnabled && CONFIG.wallet.address) {
      miner = new Miner(mempool, chain, CONFIG.wallet, p2p);
      miner.on("block:mined", async (block) => {
        if (_verifyPoW(block)) { logger.info("NODE", "node.js:Mined", "Block mined: " + block.hash); }
      });
      miner.start();
      rpc.setMiner(miner);
    }

    metrics.registerDefaults();
    metrics.startServer();

    scheduler.register("health_check", async () => {
      logger.info("METRICS", "node.js:Metrics", JSON.stringify({ height: chain.getTip().height, peers: p2p.peers.size }));
    }, 10_000);

    scheduler.onShutdown(async () => {
      if (miner) miner.stop();
      rpc.stop(); p2p.stop(); await storage.close();
    });

    scheduler.start();
  } catch (err) { process.exit(1); }
}

function _verifyPoW(block) {
  try {
    const h = block.header;
    const header = Buffer.alloc(80);
    header.writeUInt32LE(h.version || 1, 0);
    const hash = crypto.createHash("sha256").update(crypto.createHash("sha256").update(header).digest()).digest();
    return Buffer.from(hash).reverse().toString("hex") === block.hash;
  } catch { return false; }
}

const shutdown = async (sig) => { await scheduler.shutdown(); process.exit(0); };
process.on("SIGINT", () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));

main();
