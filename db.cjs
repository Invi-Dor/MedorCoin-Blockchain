/**
 * MEDORCOIN - Industrial Multi-Node Storage Engine & Auth (v5.5 Final)
 * * RESOLVED IN v5.5:
 * 1. Multi-Match Recovery: recoverPublicKey() now returns an array of all valid {recid, pubkey} pairs.
 * 2. Cryptographic Validation: Strict length/format checks (32-byte buffers) for hash, R, and S.
 * 3. Cluster-Aware CB: Circuit breaker ignores MOVED/ASK errors to prevent tripping during node failover.
 * 4. Optimized Recovery: Loop refined for performance to handle high-throughput mining validation.
 */

"use strict";

const rocksdb = require("rocksdb");
const path    = require("path");
const Redis   = require("ioredis");
const crypto  = require("crypto");
const http    = require("http");
const https   = require("https");
const secp256k1 = require("secp256k1");

class Semaphore {
  constructor(limit) {
    this._limit = limit;
    this._active = 0;
    this._queue = [];
  }
  async acquire() {
    if (this._active < this._limit) { this._active++; return; }
    return new Promise(res => this._queue.push(res));
  }
  release() {
    this._active--;
    if (this._queue.length > 0) { this._active++; this._queue.shift()(); }
  }
}

class MedorDB {
  constructor(dbPath, redisConfig, opts = {}) {
    this.nodeId = opts.nodeId || crypto.randomBytes(6).toString("hex");
    this.TAG    = `{mdr_${this.nodeId}}`;

    this.redis = Array.isArray(redisConfig?.nodes)
      ? new Redis.Cluster(redisConfig.nodes, { 
          redisOptions: { retryStrategy: t => Math.min(t * 100, 3000) },
          enableReadyCheck: true,
          scaleReads: 'slave'
        })
      : new Redis({ ...redisConfig, retryStrategy: t => Math.min(t * 100, 3000) });

    this.local = rocksdb(path.resolve(dbPath));
    this.isOpen = false;

    this.opts = {
      lockTTL: opts.lockTTL || 5000,
      lockInterval: opts.lockInterval || 1000,
      wsChannel: opts.wsChannel || "medor:mining_events",
      offlineConcurrency: opts.offlineConcurrency || 25,
      offlineMaxRetries: opts.offlineMaxRetries || 5,
      healthPort: opts.healthPort || 9191,
      metricsKey: opts.metricsKey || `sys:metrics:${this.nodeId}`,
      pushgatewayUrl: opts.pushgatewayUrl || null,
      pushInterval: opts.pushInterval || 15000,
      alertWebhook: opts.alertWebhook || null,
      cbThreshold: opts.cbThreshold || 10,
      cbResetTimeout: opts.cbResetTimeout || 30000
    };

    this.PARTITIONS = {
      STATE: `${this.TAG}state:`,
      QUEUE: `${this.TAG}offline:`,
      LOCK:  `${this.TAG}lock:`,
      WS:    `${this.TAG}ws:`,
      RETRY: `${this.TAG}retry:`,
      SESS:  `${this.TAG}session:`
    };

    this.metrics = { 
      tx: 0, err: 0, stalls: 0, ws_dropped: 0, queue_fails: 0, 
      auth_hits: 0, auth_misses: 0, cb_tripped: 0, sig_recovered: 0 
    };

    this._cbErrors = 0;
    this._cbIsTripped = false;
    this._lockTimers = new Map();
    this._wsRetryQueue = [];
    this._offlineActive = false;
    this._healthBuckets = new Map();
  }

  // ─── STARTUP ────────────────────────────────────────────────
  async open() {
    await new Promise((res, rej) => this.local.open({ create_if_missing: true }, e => e ? rej(e) : res()));
    this.isOpen = true;

    this._defineRedisScripts();
    await this._restoreMetrics();
    await this._recoverWSQueue();
    await this._recoverRetrySchedules();

    this._startWSRetryFlusher();
    this._startHealthServer();
    this._startMetricsPersistence();
    if (this.opts.pushgatewayUrl) this._startPushgateway();

    this._log("INFO", `Node ${this.nodeId} v5.5 (Resilience) Online.`);
  }

  _defineRedisScripts() {
    this.redis.defineCommand("getAndExtend", {
      numberOfKeys: 1,
      lua: `
        local val = redis.call('get', KEYS[1])
        if val then
            redis.call('pexpire', KEYS[1], ARGV[1])
            return val
        else return nil end
      `
    });
  }

  // ─── CRYPTOGRAPHIC RECOVERY (v5.5 Resolved Multi-Match & Validation) ─
  recoverPublicKey(hash, r, s) {
    // Strict Input Validation
    if (!Buffer.isBuffer(hash) || hash.length !== 32) return null;
    if (!Buffer.isBuffer(r) || r.length !== 32) return null;
    if (!Buffer.isBuffer(s) || s.length !== 32) return null;

    try {
      const sig = Buffer.concat([r, s]);
      const matches = [];

      for (let recid = 0; recid < 4; recid++) {
        try {
          const pubkey = secp256k1.ecdsaRecover(sig, recid, hash, false);
          if (pubkey) {
            matches.push({ recid, pubkey: Buffer.from(pubkey).toString('hex') });
          }
        } catch (e) { continue; } // Invalid recovery ID for this signature
      }

      if (matches.length > 0) {
        this.metrics.sig_recovered += matches.length;
        return matches;
      }
    } catch (err) {
      this._alert("CRYPTO_RECOVERY_FATAL", { err: err.message });
    }
    return null;
  }

  // ─── AUTHENTICATION (v5.5 Resolved Cluster-Aware CB) ────────
  getAuthMiddleware() {
    return async (req, res, next) => {
      this.metrics.auth_hits++;

      if (this._cbIsTripped) {
        return res.status(503).json({ error: "Auth Subsystem Offline" });
      }

      try {
        const token = this._extractToken(req);
        if (!token || token.length < 32) {
          this.metrics.auth_misses++;
          return res.status(401).json({ error: "Invalid Credentials" });
        }

        const raw = await this.redis.getAndExtend(this.PARTITIONS.SESS + token, 3600000);
        if (!raw) { this.metrics.auth_misses++; return res.status(401).json({ error: "Session Expired" }); }

        req.user = JSON.parse(raw);
        this._cbErrors = Math.max(0, this._cbErrors - 1);
        next();
      } catch (err) {
        this._handleCBError(err);
        res.status(500).json({ error: "Auth Failure" });
      }
    };
  }

  _extractToken(req) {
    const auth = req.headers['authorization'];
    if (auth?.startsWith('Bearer ')) return auth.slice(7);
    if (req.query?.token) return req.query.token;
    const cookies = req.headers['cookie'];
    if (cookies) {
      const match = cookies.match(/(?:^|; )session_token=([^;]*)/);
      if (match) return match[1];
    }
    return null;
  }

  _handleCBError(err) {
    // Ignore Cluster Redirections (Normal Failover behavior)
    if (err?.message?.startsWith('MOVED') || err?.message?.startsWith('ASK')) return;

    this._cbErrors++;
    if (this._cbErrors >= this.opts.cbThreshold && !this._cbIsTripped) {
      this._cbIsTripped = true;
      this.metrics.cb_tripped++;
      this._alert("CIRCUIT_BREAKER_TRIPPED", { err: err.message });
      setTimeout(() => { this._cbIsTripped = false; this._cbErrors = 0; }, this.opts.cbResetTimeout);
    }
  }

  // ─── LOCKING (Jittered) ─────────────────────────────────────
  async withLock(resource, ttl, fn) {
    const key = this.PARTITIONS.LOCK + resource;
    const token = `${this.nodeId}:${crypto.randomBytes(4).toString('hex')}`;
    const actualTTL = ttl || this.opts.lockTTL;

    const ok = await this.redis.set(key, token, "NX", "PX", actualTTL);
    if (!ok) throw new Error("LOCK_HELD");

    const interval = setInterval(async () => {
      try {
        await _sleep(Math.random() * 200); // Jitter
        const remote = await this.redis.get(key);
        if (remote === token) await this.redis.pexpire(key, actualTTL);
        else clearInterval(interval);
      } catch { clearInterval(interval); }
    }, this.opts.lockInterval);

    try { return await fn(); }
    finally {
      clearInterval(interval);
      await this.redis.eval("if redis.call('get',KEYS[1])==ARGV[1] then return redis.call('del',KEYS[1]) else return 0 end", 1, key, token).catch(()=>{});
    }
  }

  // ─── OFFLINE QUEUE (Batch Persistence) ─────────────────────
  async _scheduleRetry(id, data, delayMs) {
    const rid = id.replace(this.PARTITIONS.QUEUE, this.PARTITIONS.RETRY);
    const meta = { originalId: id, data, retryAt: Date.now() + delayMs };
    
    const batch = this.local.batch();
    batch.put(rid, JSON.stringify(meta));
    await new Promise((rs, rj) => batch.write(e => e ? rj(e) : rs()));

    setTimeout(async () => {
      const b = this.local.batch();
      b.put(id, JSON.stringify(data));
      b.del(rid);
      await new Promise(rs => b.write(rs));
    }, delayMs);
  }

  async _recoverRetrySchedules() {
    const stream = this.streamByPrefix(this.PARTITIONS.RETRY);
    for await (const [rid, meta] of stream) {
      const rem = Math.max(0, meta.retryAt - Date.now());
      setTimeout(async () => {
        await this._localPut(meta.originalId, meta.data);
        await this._localDel(rid);
      }, rem);
    }
  }

  // ─── CORE PIPELINES ─────────────────────────────────────────
  async executeTransaction(updates, removals = []) {
    const pipe = this.redis.pipeline();
    for (const [k, v] of Object.entries(updates)) pipe.set(k, JSON.stringify(v));
    for (const k of removals) pipe.del(k);

    const res = await pipe.exec();
    if (res?.some(r => r[0])) throw new Error("REDIS_TX_FAIL");

    try {
      const b = this.local.batch();
      for (const [k, v] of Object.entries(updates)) b.put(k, JSON.stringify(v));
      for (const k of removals) b.del(k);
      await new Promise((rs, rj) => b.write(e => e ? rj(e) : rs()));
      this.metrics.tx++;
    } catch (err) {
      await this._alert("TX_INCONSISTENCY", { err: err.message });
      throw err;
    }
  }

  async *streamByPrefix(prefix) {
    const it = this.local.iterator({ gte: prefix });
    try {
      while (true) {
        const entry = await new Promise((res, rej) => it.next((e, k, v) => e ? rej(e) : res(k ? [k, v] : null)));
        if (!entry || !entry[0].toString().startsWith(prefix)) break;
        yield [entry[0].toString(), JSON.parse(entry[1].toString())];
      }
    } finally { await new Promise(res => it.end(res)); }
  }

  async _recoverWSQueue() {
    const stream = this.streamByPrefix(this.PARTITIONS.WS);
    for await (const [id, ev] of stream) this._wsRetryQueue.push({ id, event: ev });
  }

  _startWSRetryFlusher() {
    setInterval(async () => {
      if (this._wsRetryQueue.length === 0) return;
      const b = this._wsRetryQueue.splice(0, 50);
      for (const item of b) {
        try {
          await this.redis.publish(this.opts.wsChannel, JSON.stringify(item.event));
          await this._localDel(item.id);
        } catch { this._wsRetryQueue.push(item); }
      }
    }, 2500);
  }

  async _alert(ev, detail) {
    this._log("ALERT", ev, detail);
    if (!this.opts.alertWebhook) return;
    const body = JSON.stringify({ ev, node: this.nodeId, detail, ts: Date.now() });
    _httpPost(this.opts.alertWebhook, body, "application/json").catch(()=>{});
  }

  _log(level, msg, meta = {}) {
    process.stdout.write(JSON.stringify({ ts: new Date().toISOString(), level, node: this.nodeId, msg, ...meta }) + "\n");
  }

  _startMetricsPersistence() {
    setInterval(() => this._localPut(this.opts.metricsKey, this.metrics).catch(()=>{}), 30000);
  }

  async _restoreMetrics() {
    const m = await this._localGet(this.opts.metricsKey);
    if (m) this.metrics = { ...this.metrics, ...m };
  }

  _startHealthServer() {
    http.createServer((req, res) => {
      if (req.url === "/metrics") {
        res.writeHead(200, { "Content-Type": "text/plain" });
        return res.end(Object.entries(this.metrics).map(([k,v]) => `medor_${k} ${v}`).join("\n"));
      }
      res.writeHead(200); res.end(JSON.stringify({ ok: this.isOpen, cb: this._cbIsTripped }));
    }).listen(this.opts.healthPort);
  }

  async _localPut(k, v) { return new Promise((rs, rj) => this.local.put(k, JSON.stringify(v), e => e ? rj(e) : rs())); }
  async _localDel(k) { return new Promise((rs, rj) => this.local.del(k, e => e ? rj(e) : rs())); }
  async _localGet(k) { return new Promise(res => this.local.get(k, (e, v) => res(e ? null : JSON.parse(v.toString())))); }
}

function _httpPost(urlStr, body, contentType) {
  return new Promise((resolve, reject) => {
    const u = new URL(urlStr);
    const req = (u.protocol === "https:" ? https : http).request({
      hostname: u.hostname, path: u.pathname, method: "POST",
      headers: { "Content-Type": contentType, "Content-Length": Buffer.byteLength(body) }
    }, res => { res.resume(); resolve(); });
    req.on("error", reject); req.write(body); req.end();
  });
}

const _sleep = ms => new Promise(r => setTimeout(r, ms));

module.exports = MedorDB;
