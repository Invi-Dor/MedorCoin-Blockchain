// db.cjs
/**
 * MEDORCOIN - Industrial Multi-Node Storage Engine (v5.2 Final)
 *
 * Fixes v5.2:
 *  1. Crash-safe retry metadata — persisted to RocksDB before setTimeout
 *  2. Pushgateway failure logging — errors surfaced with structured alert
 *  3. Streaming batch offline queue — stream + concurrency semaphore
 *  4. Structured error logging — all failures routed through _alert()
 *  5. Redis pipeline retry — exponential backoff on pipeline.exec() failures
 */

"use strict";

const rocksdb = require("rocksdb");
const path    = require("path");
const Redis   = require("ioredis");
const crypto  = require("crypto");
const http    = require("http");
const https   = require("https");

// ─────────────────────────────────────────────────────────────────
// FIX 5: Semaphore — limits concurrency on streaming batch processor
// ─────────────────────────────────────────────────────────────────
class Semaphore {
  constructor(limit) {
    this._limit   = limit;
    this._active  = 0;
    this._queue   = [];
  }
  acquire() {
    return new Promise((resolve) => {
      if (this._active < this._limit) {
        this._active++;
        resolve();
      } else {
        this._queue.push(resolve);
      }
    });
  }
  release() {
    this._active--;
    if (this._queue.length > 0) {
      this._active++;
      this._queue.shift()();
    }
  }
}

class MedorDB {
  constructor(dbPath, redisConfig, opts = {}) {
    this.nodeId = opts.nodeId || crypto.randomBytes(6).toString("hex");
    this.TAG    = "{mdr_" + this.nodeId + "}";

    this.redis = Array.isArray(redisConfig?.nodes)
      ? new Redis.Cluster(redisConfig.nodes, {
          redisOptions: { retryStrategy: t => Math.min(t * 100, 3000) }
        })
      : new Redis({ ...redisConfig, retryStrategy: t => Math.min(t * 100, 3000) });

    this.local  = rocksdb(path.resolve(dbPath));
    this.isOpen = false;

    this.opts = {
      lockTTL:             opts.lockTTL             || 5000,
      lockInterval:        opts.lockInterval         || 1000,
      wsChannel:           opts.wsChannel            || "medor:mining_events",
      wsRetryLimit:        opts.wsRetryLimit         || 100,
      offlineConcurrency:  opts.offlineConcurrency   || 20,
      offlineMaxRetries:   opts.offlineMaxRetries    || 3,
      healthPort:          opts.healthPort           || 9191,
      healthToken:         opts.healthToken          || null,
      metricsKey:          opts.metricsKey           || ("sys:metrics:" + this.nodeId),
      pushgatewayUrl:      opts.pushgatewayUrl       || null,
      pushInterval:        opts.pushInterval         || 15000,
      alertWebhook:        opts.alertWebhook         || null,   // FIX 4
      // FIX 5: pipeline retry config
      pipelineMaxRetries:  opts.pipelineMaxRetries   || 3,
      pipelineRetryBaseMs: opts.pipelineRetryBaseMs  || 200,
    };

    this.PARTITIONS = {
      STATE: this.TAG + "state:",
      QUEUE: this.TAG + "offline:",
      LOCK:  this.TAG + "lock:",
      WS:    this.TAG + "ws:",
      RETRY: this.TAG + "retry:", // FIX 1: persisted retry metadata partition
    };

    this._lockTimers     = new Map();
    this._wsRetryQueue   = [];
    this._offlineActive  = false;
    this.metrics         = {
      tx: 0, err: 0, stalls: 0,
      ws_dropped: 0, queue_fails: 0,
      pipeline_retries: 0,      // FIX 5
      pushgateway_errors: 0,    // FIX 2
      alert_sent: 0,            // FIX 4
    };
    this._healthBuckets  = new Map();
  }

  // ── Open ─────────────────────────────────────────────────────
  async open() {
    await new Promise((res, rej) =>
      this.local.open({ create_if_missing: true }, e => e ? rej(e) : res())
    );
    this.isOpen = true;

    await this._restoreMetrics();
    await this._recoverWSQueue();
    // FIX 1: recover any persisted retry schedules that survived a crash
    await this._recoverRetrySchedules();

    this._startWSRetryFlusher();
    this._startHealthServer();
    this._startCleanupInterval();
    this._startMetricsPersistence();
    if (this.opts.pushgatewayUrl) this._startPushgateway();

    this._log("INFO", "Node " + this.nodeId + " Online v5.2");
  }

  // ─────────────────────────────────────────────────────────────
  // FIX 5: Redis pipeline with exponential backoff retry
  // ─────────────────────────────────────────────────────────────
  async _execPipelineWithRetry(pipeline) {
    const maxRetries = this.opts.pipelineMaxRetries;
    const baseMs     = this.opts.pipelineRetryBaseMs;
    let   lastErr    = null;

    for (let attempt = 0; attempt <= maxRetries; attempt++) {
      try {
        const res = await pipeline.exec();
        // Check for per-command errors in result
        if (res && res.some(r => r[0])) {
          throw new Error("PIPELINE_PARTIAL_ERROR: " +
            res.filter(r => r[0]).map(r => r[0].message).join(", "));
        }
        if (attempt > 0) {
          this.metrics.pipeline_retries += attempt;
          this._log("INFO", "Pipeline succeeded after " + attempt + " retries");
        }
        return res;
      } catch (err) {
        lastErr = err;
        this.metrics.err++;
        if (attempt < maxRetries) {
          const delay = baseMs * Math.pow(2, attempt);
          this._log("WARN", "Pipeline attempt " + (attempt + 1) + " failed — retry in " +
            delay + "ms: " + err.message);
          await _sleep(delay);
        }
      }
    }

    // All retries exhausted
    this.metrics.pipeline_retries += maxRetries;
    await this._alert("PIPELINE_EXHAUSTED", { err: lastErr?.message });
    throw lastErr;
  }

  // ─────────────────────────────────────────────────────────────
  // Transaction engine — uses retry pipeline
  // ─────────────────────────────────────────────────────────────
  async executeTransaction(updates, removals = []) {
    const pipeline = this.redis.pipeline();
    const keys     = [...Object.keys(updates), ...removals];

    const rawBackups = keys.length > 0 ? await this.redis.mget(...keys) : [];
    const backups    = new Map(keys.map((k, i) => [k, rawBackups[i]]));

    for (const [k, v] of Object.entries(updates)) {
      if (!k.startsWith(this.TAG)) throw new Error("ATOMIC_VIOLATION: " + k);
      pipeline.set(k, JSON.stringify(v));
    }
    for (const k of removals) {
      if (!k.startsWith(this.TAG)) throw new Error("ATOMIC_VIOLATION: " + k);
      pipeline.del(k);
    }

    // FIX 5: use retry pipeline instead of plain exec()
    await this._execPipelineWithRetry(pipeline);

    try {
      const batch = this.local.batch();
      for (const [k, v] of Object.entries(updates)) batch.put(k, JSON.stringify(v));
      for (const k of removals) batch.del(k);
      await new Promise((rs, rj) => batch.write(e => e ? rj(e) : rs()));
      this.metrics.tx++;
    } catch (err) {
      const rollback = this.redis.pipeline();
      for (const [k, v] of backups) v ? rollback.set(k, v) : rollback.del(k);
      await this._execPipelineWithRetry(rollback).catch(() => {});
      await this._alert("TX_ROLLBACK", { err: err.message, keys });
      throw err;
    }
  }

  // ─────────────────────────────────────────────────────────────
  // Locking
  // ─────────────────────────────────────────────────────────────
  async withLock(resource, ttl, fn) {
    const key        = this.PARTITIONS.LOCK + resource;
    const ownerToken = this.nodeId + ":" + crypto.randomBytes(8).toString("hex");
    let   currentTTL = ttl || this.opts.lockTTL;

    const acquired = await this.redis.set(key, ownerToken, "NX", "PX", currentTTL);
    if (!acquired) throw new Error("LOCK_BUSY: " + resource);

    let lastTick = Date.now();
    const interval = setInterval(async () => {
      try {
        const now = Date.now();
        const lag = now - lastTick - this.opts.lockInterval;
        lastTick  = now;
        if (lag > 250) {
          this.metrics.stalls++;
          currentTTL = Math.min(currentTTL + lag * 2, 60000);
        }
        const remote = await this.redis.get(key);
        if (remote === ownerToken) await this.redis.pexpire(key, currentTTL);
        else clearInterval(interval);
      } catch {}
    }, this.opts.lockInterval);

    this._lockTimers.set(key, interval);
    try {
      return await fn();
    } finally {
      clearInterval(interval);
      this._lockTimers.delete(key);
      await this.redis.eval(
        "if redis.call('get',KEYS[1])==ARGV[1] then return redis.call('del',KEYS[1]) else return 0 end",
        1, key, ownerToken
      ).catch(() => {});
    }
  }

  // ─────────────────────────────────────────────────────────────
  // FIX 1: Crash-safe offline queue
  // Retry metadata is written to RocksDB BEFORE scheduling setTimeout.
  // On crash + restart, _recoverRetrySchedules() re-queues them.
  // ─────────────────────────────────────────────────────────────
  async pushOfflineTask(task) {
    const id = this.PARTITIONS.QUEUE +
      Date.now() + ":" + crypto.randomBytes(4).toString("hex");
    await this._localPut(id, { task, attempts: 0 });
  }

  async _scheduleRetry(id, data, delayMs) {
    // FIX 1: persist retry schedule to RocksDB FIRST — survives crash
    const retryKey = this.PARTITIONS.RETRY + id.split(this.PARTITIONS.QUEUE)[1];
    await this._localPut(retryKey, {
      originalId: id,
      data,
      retryAfter: Date.now() + delayMs,
    });

    // Now schedule in memory
    setTimeout(async () => {
      await this._localPut(id, data);
      await this._localDel(retryKey); // clear retry schedule record
    }, delayMs);
  }

  async _recoverRetrySchedules() {
    // FIX 1: on startup, re-schedule any persisted retries that survived crash
    let recovered = 0;
    try {
      const stream = this.streamByPrefix(this.PARTITIONS.RETRY);
      for await (const [retryKey, meta] of stream) {
        const remainingMs = Math.max(0, meta.retryAfter - Date.now());
        setTimeout(async () => {
          try {
            await this._localPut(meta.originalId, meta.data);
            await this._localDel(retryKey);
          } catch (err) {
            await this._alert("RETRY_RECOVER_FAIL",
              { retryKey, err: err.message });
          }
        }, remainingMs);
        recovered++;
      }
      if (recovered > 0) {
        this._log("INFO", "Recovered " + recovered + " retry schedules from crash");
      }
    } catch (err) {
      // FIX 4: route through _alert instead of console
      await this._alert("RETRY_RECOVER_ERROR", { err: err.message });
    }
  }

  // ─────────────────────────────────────────────────────────────
  // FIX 3: Streaming batch offline queue with Semaphore
  // Uses async generator stream + semaphore concurrency control.
  // Never loads entire queue into memory.
  // ─────────────────────────────────────────────────────────────
  async processOfflineQueue(handler) {
    if (this._offlineActive) return;
    this._offlineActive = true;

    const sem = new Semaphore(this.opts.offlineConcurrency);
    const inFlight = [];

    try {
      const stream = this.streamByPrefix(this.PARTITIONS.QUEUE);

      for await (const [id, data] of stream) {
        await sem.acquire();

        const task = (async () => {
          try {
            await this._runTaskWithRetry(id, data, handler);
          } finally {
            sem.release();
          }
        })();

        inFlight.push(task);

        // Drain completed tasks to prevent unbounded array growth
        if (inFlight.length >= this.opts.offlineConcurrency * 4) {
          await Promise.allSettled(inFlight.splice(0));
        }
      }

      // Wait for all remaining in-flight tasks
      await Promise.allSettled(inFlight);

    } finally {
      this._offlineActive = false;
    }
  }

  async _runTaskWithRetry(id, data, handler) {
    try {
      await handler(data.task);
      await this._localDel(id);
    } catch (err) {
      data.attempts++;

      if (data.attempts >= this.opts.offlineMaxRetries) {
        this.metrics.queue_fails++;
        // FIX 4: structured alert instead of console.error
        await this._alert("TASK_PERMANENTLY_FAILED",
          { id, attempts: data.attempts, err: err.message });
        await this._localDel(id);
      } else {
        const delayMs = Math.pow(2, data.attempts) * 1000;
        // FIX 1: crash-safe retry persistence
        await this._scheduleRetry(id, data, delayMs);
      }
    }
  }

  // ─────────────────────────────────────────────────────────────
  // WS publish with retry queue
  // ─────────────────────────────────────────────────────────────
  async publish(event) {
    try {
      await this.redis.publish(this.opts.wsChannel, JSON.stringify(event));
    } catch (err) {
      // FIX 4: log failure through structured alert
      await this._alert("WS_PUBLISH_FAIL",
        { event: event?.type, err: err.message });
      const id = this.PARTITIONS.WS +
        Date.now() + ":" + crypto.randomBytes(4).toString("hex");
      await this._localPut(id, event);
      this._wsRetryQueue.push({ id, event });
    }
  }

  _startWSRetryFlusher() {
    setInterval(async () => {
      if (!this._wsRetryQueue.length) return;
      const batch = this._wsRetryQueue.splice(0, this.opts.wsRetryLimit);
      for (const item of batch) {
        try {
          await this.redis.publish(this.opts.wsChannel, JSON.stringify(item.event));
          await this._localDel(item.id);
        } catch {
          if (this._wsRetryQueue.length < 1000) {
            this._wsRetryQueue.push(item);
          } else {
            this.metrics.ws_dropped++;
            await this._alert("WS_RETRY_DROPPED",
              { event: item.event?.type, queueSize: this._wsRetryQueue.length });
          }
        }
      }
    }, 2000);
  }

  // ─────────────────────────────────────────────────────────────
  // FIX 2: Pushgateway with failure logging
  // ─────────────────────────────────────────────────────────────
  _startPushgateway() {
    setInterval(async () => {
      const payload = this._renderPrometheus();
      const url     = this.opts.pushgatewayUrl +
        "/metrics/job/medordb/instance/" + this.nodeId;
      try {
        await _httpPost(url, payload, "text/plain");
      } catch (err) {
        // FIX 2: no longer silent — logs + increments metric + alerts
        this.metrics.pushgateway_errors++;
        this._log("WARN", "Pushgateway push failed: " + err.message);
        // Alert only every 5 failures to avoid webhook spam
        if (this.metrics.pushgateway_errors % 5 === 0) {
          await this._alert("PUSHGATEWAY_DEGRADED", {
            url,
            failures: this.metrics.pushgateway_errors,
            err:      err.message,
          });
        }
      }
    }, this.opts.pushInterval);
  }

  // ─────────────────────────────────────────────────────────────
  // FIX 4: Structured alert — webhook + structured log
  // Replaces all bare console.error calls throughout the file.
  // ─────────────────────────────────────────────────────────────
  async _alert(event, detail = {}) {
    this.metrics.alert_sent++;
    this._log("ALERT", event, detail);

    if (!this.opts.alertWebhook) return;
    const payload = JSON.stringify({
      event,
      node:   this.nodeId,
      ts:     new Date().toISOString(),
      detail,
    });
    try {
      await _httpPost(this.opts.alertWebhook, payload, "application/json");
    } catch (err) {
      // Never throw from alert — just log to stderr
      process.stderr.write("[MedorDB] Alert webhook failed: " + err.message + "\n");
    }
  }

  _log(level, msg, meta = {}) {
    process.stdout.write(JSON.stringify({
      ts:     new Date().toISOString(),
      level,
      node:   this.nodeId,
      module: "MedorDB",
      msg,
      ...meta,
    }) + "\n");
  }

  // ─────────────────────────────────────────────────────────────
  // Metrics persistence
  // ─────────────────────────────────────────────────────────────
  _startMetricsPersistence() {
    setInterval(async () => {
      try {
        await this._localPut(this.opts.metricsKey,
          { ...this.metrics, ts: Date.now() });
      } catch (err) {
        // FIX 4: structured alert
        await this._alert("METRICS_FLUSH_FAIL", { err: err.message });
      }
    }, 30000);
  }

  async _restoreMetrics() {
    try {
      const saved = await this._localGet(this.opts.metricsKey);
      if (saved) {
        this.metrics = { ...this.metrics, ...saved };
        this._log("INFO", "Metrics restored", { ts: saved.ts });
      }
    } catch (err) {
      await this._alert("METRICS_RESTORE_FAIL", { err: err.message });
    }
  }

  // ─────────────────────────────────────────────────────────────
  // Health server
  // ─────────────────────────────────────────────────────────────
  _startHealthServer() {
    http.createServer((req, res) => {
      const ip = req.socket.remoteAddress || "unknown";
      if (!this._checkRateLimit(ip)) {
        res.writeHead(429, { "Content-Type": "text/plain" });
        return res.end("Too Many Requests");
      }

      if (req.url === "/health" || req.url === "/metrics") {
        if (this.opts.healthToken) {
          const auth = (req.headers["authorization"] || "").replace("Bearer ", "");
          if (!this._safeCompare(auth, this.opts.healthToken)) {
            res.writeHead(401, { "Content-Type": "text/plain" });
            return res.end("Unauthorized");
          }
        }

        if (req.url === "/metrics") {
          res.writeHead(200, { "Content-Type": "text/plain" });
          return res.end(this._renderPrometheus());
        }

        res.writeHead(200, {
          "Content-Type":  "application/json",
          "Cache-Control": "no-store",
        });
        return res.end(JSON.stringify({
          node:    this.nodeId,
          ok:      this.isOpen,
          ts:      new Date().toISOString(),
          metrics: this.metrics,
          redis:   this.redis.status || "unknown",
          wsQueue: this._wsRetryQueue.length,
        }));
      }

      res.writeHead(404);
      res.end("Not found");
    }).listen(this.opts.healthPort, () => {
      this._log("INFO", "Health server on port " + this.opts.healthPort);
    });
  }

  _renderPrometheus() {
    return Object.entries(this.metrics)
      .filter(([, v]) => typeof v === "number")
      .map(([k, v]) => "medordb_" + k + "{node=\"" + this.nodeId + "\"} " + v)
      .join("\n") + "\n";
  }

  // ─────────────────────────────────────────────────────────────
  // WS queue recovery on boot
  // ─────────────────────────────────────────────────────────────
  async _recoverWSQueue() {
    let recovered = 0;
    try {
      const stream = this.streamByPrefix(this.PARTITIONS.WS);
      for await (const [id, event] of stream) {
        this._wsRetryQueue.push({ id, event });
        recovered++;
      }
      if (recovered > 0) {
        this._log("INFO", "Recovered " + recovered + " WS events from crash");
      }
    } catch (err) {
      // FIX 4: structured alert
      await this._alert("WS_RECOVER_ERROR", { err: err.message });
    }
  }

  // ─────────────────────────────────────────────────────────────
  // Token bucket rate limiter for health endpoint
  // ─────────────────────────────────────────────────────────────
  _checkRateLimit(ip) {
    const now = Date.now();
    let b     = this._healthBuckets.get(ip) || { tokens: 10, last: now };
    b.tokens  = Math.min(10, b.tokens + (now - b.last) / 500);
    b.last    = now;
    if (b.tokens < 1) return false;
    b.tokens -= 1;
    this._healthBuckets.set(ip, b);
    return true;
  }

  _safeCompare(a, b) {
    if (!a || !b) return false;
    try {
      const ba = Buffer.from(String(a));
      const bb = Buffer.from(String(b));
      if (ba.length !== bb.length) return false;
      return crypto.timingSafeEqual(ba, bb);
    } catch { return false; }
  }

  _startCleanupInterval() {
    setInterval(() => {
      const now = Date.now();
      for (const [ip, b] of this._healthBuckets.entries()) {
        if (now - b.last > 600000) this._healthBuckets.delete(ip);
      }
    }, 600000);
  }

  // ─────────────────────────────────────────────────────────────
  // Async generator stream
  // ─────────────────────────────────────────────────────────────
  async *streamByPrefix(prefix) {
    const it = this.local.iterator({ gte: prefix });
    try {
      while (true) {
        const entry = await new Promise((res, rej) =>
          it.next((e, k, v) => e ? rej(e) : res(k ? [k, v] : null))
        );
        if (!entry || !entry[0].toString().startsWith(prefix)) break;
        yield [entry[0].toString(), JSON.parse(entry[1].toString())];
      }
    } finally {
      await new Promise(res => it.end(res));
    }
  }

  // ─────────────────────────────────────────────────────────────
  // RocksDB primitives
  // ─────────────────────────────────────────────────────────────
  async _localPut(k, v) {
    return new Promise((rs, rj) =>
      this.local.put(k, JSON.stringify(v), e => e ? rj(e) : rs()));
  }
  async _localDel(k) {
    return new Promise((rs, rj) =>
      this.local.del(k, e => e ? rj(e) : rs()));
  }
  async _localGet(k) {
    return new Promise(res =>
      this.local.get(k, (e, v) =>
        res(e ? null : (() => { try { return JSON.parse(v.toString()); } catch { return null; } })())
      )
    );
  }

  async close() {
    for (const t of this._lockTimers.values()) clearInterval(t);
    await this._localPut(this.opts.metricsKey,
      { ...this.metrics, flushedAt: Date.now() });
    await new Promise(res => this.local.close(res));
    this._log("INFO", "MedorDB closed cleanly.");
  }
}

// ─────────────────────────────────────────────────────────────────
// HTTP POST helper
// ─────────────────────────────────────────────────────────────────
function _httpPost(urlStr, body, contentType) {
  return new Promise((resolve, reject) => {
    const u      = new URL(urlStr);
    const driver = u.protocol === "https:" ? https : http;
    const req    = driver.request({
      hostname: u.hostname,
      port:     u.port || (u.protocol === "https:" ? 443 : 80),
      path:     u.pathname,
      method:   "POST",
      headers:  {
        "Content-Type":   contentType,
        "Content-Length": Buffer.byteLength(body),
      },
      timeout: 5000,
    }, (res) => { res.resume(); resolve(); });
    req.on("timeout", () => { req.destroy(); reject(new Error("Timeout")); });
    req.on("error", reject);
    req.write(body);
    req.end();
  });
}

function _sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

module.exports = MedorDB;
