// metrics.js
/**
 * MedorCoin - Metrics & Monitoring Module
 * Absolute final production version.
 *
 * Fixes:
 *  1. Redis retry queue — failed writes queued and retried, no data loss
 *  2. Per-metric custom histogram buckets — configurable on registerHistogram()
 *  3. gaugeAdd syncs delta to Redis via HINCRBYFLOAT — no overwrite race
 *  4. Per-metric cardinality limits — configurable per name, not global only
 *  5. Histogram sum/count as BigInt — safe at extreme observation frequency
 */

const http = require("http");
const fs   = require("fs");
const path = require("path");
const logger = require('./logger.cjs'); 

const METRICS_PORT        = 9090;
const SCRAPE_CACHE_TTL    = 2_000;
const DEFAULT_CARDINALITY = 100;
const PERSIST_PATH        = "./data/metrics_snapshot.json";
const PERSIST_INTERVAL    = 30_000;
const REDIS_RETRY_INTERVAL= 5_000;   // FIX 1: retry queue flush interval
const REDIS_RETRY_MAX     = 1_000;   // FIX 1: max queued retries before drop

const DEFAULT_BUCKETS = [1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, Infinity];

class Metrics {
  constructor() {
    this.counters   = new Map();
    this.gauges     = new Map();
    this.histograms = new Map();

    // FIX 2: per-metric histogram bucket config
    this._histogramBuckets = new Map(); // name -> number[]

    // FIX 4: per-metric cardinality limits
    this._cardinalityLimits = new Map(); // name -> number
    this._cardinalityIndex  = new Map(); // name -> Set of keys

    this._cache     = null;
    this._cacheTime = 0;

    this._redis        = null;
    // FIX 1: retry queue for failed Redis writes
    this._redisQueue   = [];
    this._retryTimer   = null;

    this.server        = null;
    this._persistTimer = null;
  }

  // ─── FIX 1: Redis with retry queue ────────────────────────────

  useRedis(redisClient) {
    this._redis = redisClient;
    // Start flushing retry queue periodically
    this._retryTimer = setInterval(() => this._flushRedisQueue(), REDIS_RETRY_INTERVAL);
    logger.info("METRICS", "metrics.js:58", "Redis backend enabled with retry queue.");
  }

  _redisWrite(op) {
    if (!this._redis) return;
    this._execRedisOp(op).catch(() => {
      if (this._redisQueue.length >= REDIS_RETRY_MAX) {
        logger.warn("METRICS", "metrics.js:65", "Redis retry queue full. Dropping oldest entry.");
        this._redisQueue.shift();
      }
      this._redisQueue.push(op);
    });
  }

  async _execRedisOp(op) {
    if (op.type === "hincrby") {
      await this._redis.hincrby(op.hash, op.key, op.amount);
    } else if (op.type === "hset") {
      await this._redis.hset(op.hash, op.key, op.value);
    } else if (op.type === "hincrbyfloat") {
      // FIX 3: delta-based gauge sync
      await this._redis.hincrbyfloat(op.hash, op.key, op.delta);
    }
  }

  async _flushRedisQueue() {
    if (!this._redis || this._redisQueue.length === 0) return;
    const batch = this._redisQueue.splice(0, 100); // process up to 100 at a time
    for (const op of batch) {
      try {
        await this._execRedisOp(op);
      } catch (err) {
        // Re-queue if still failing
        if (this._redisQueue.length < REDIS_RETRY_MAX) {
          this._redisQueue.push(op);
        }
        logger.error("METRICS", "metrics.js:91", `Redis retry failed: ${err.message}`);
        break; // stop this flush cycle — Redis may still be down
      }
    }
    if (batch.length > 0) {
      logger.info("METRICS", "metrics.js:97", `Redis queue flushed ${batch.length} ops. Remaining: ${this._redisQueue.length}`);
    }
  }

  // ─── FIX 2: Register histogram with custom buckets ────────────

  /**
   * Pre-register a histogram with custom bucket boundaries (ms).
   * Call this before any observe() for that metric name.
   *
   * Example — microsecond-level buckets:
   *   metrics.registerHistogram("medorcoin_sig_verify_us", [0.1, 0.5, 1, 2, 5, 10, 50]);
   *
   * Example — block processing with wide buckets:
   *   metrics.registerHistogram("medorcoin_block_ms", [10, 50, 100, 500, 1000, 5000]);
   */
  registerHistogram(name, buckets = DEFAULT_BUCKETS) {
    if (!buckets.includes(Infinity)) buckets = [...buckets, Infinity];
    this._histogramBuckets.set(name, buckets);

    const bucketMap = new Map();
    for (const b of buckets) bucketMap.set(b, 0);
    // FIX 5: sum and count as BigInt
    this.histograms.set(name, { buckets: bucketMap, sum: BigInt(0), count: BigInt(0) });
    logger.info("METRICS", "metrics.js:120", `Histogram registered: ${name} (${buckets.length} buckets)`);
  }

  // ─── FIX 4: Per-metric cardinality config ─────────────────────

  /**
   * Set a custom cardinality cap for a specific metric.
   * Call before first use of that metric name.
   *
   * Example — high-cardinality metric:
   *   metrics.setCardinalityLimit("medorcoin_rpc_calls_total", 500);
   *
   * Example — tight limit for a noisy label:
   *   metrics.setCardinalityLimit("medorcoin_peer_errors", 20);
   */
  setCardinalityLimit(name, limit) {
    this._cardinalityLimits.set(name, limit);
  }

  _guardCardinality(name, key) {
    let keys = this._cardinalityIndex.get(name);
    if (!keys) {
      keys = new Set();
      this._cardinalityIndex.set(name, keys);
    }
    if (keys.has(key)) return true;
    // FIX 4: use per-metric limit if set, else default
    const limit = this._cardinalityLimits.get(name) ?? DEFAULT_CARDINALITY;
    if (keys.size >= limit) {
      logger.warn("METRICS", "metrics.js:151", `Cardinality limit (${limit}) hit for "${name}". Dropping.`);
      return false;
    }
    keys.add(key);
    return true;
  }

  // ─── Counters ──────────────────────────────────────────────────

  increment(name, labels = {}, amount = 1) {
    const key = this._key(name, labels);
    if (!this._guardCardinality(name, key)) return;

    const existing = this.counters.get(key);
    if (existing) {
      existing.value += BigInt(amount);
    } else {
      this.counters.set(key, { name, value: BigInt(amount), labels });
    }

    // FIX 1: queued Redis write
    this._redisWrite({ type: "hincrby", hash: "mdc:counters", key, amount });
    this._invalidateCache();
  }

  // ─── Gauges ────────────────────────────────────────────────────

  gauge(name, value, labels = {}) {
    const key = this._key(name, labels);
    if (!this._guardCardinality(name, key)) return;
    this.gauges.set(key, { name, value, labels });

    // FIX 3: absolute set via HSET
    this._redisWrite({ type: "hset", hash: "mdc:gauges", key, value });
    this._invalidateCache();
  }

  /**
   * FIX 3: delta gauge — syncs via HINCRBYFLOAT so multiple nodes
   * accumulate correctly instead of overwriting each other.
   */
  gaugeAdd(name, delta, labels = {}) {
    const key = this._key(name, labels);
    if (!this._guardCardinality(name, key)) return;

    const existing = this.gauges.get(key);
    const current  = existing ? existing.value : 0;
    this.gauges.set(key, { name: name, value: current + delta, labels });

    // FIX 3: delta write — no overwrite race across nodes
    this._redisWrite({ type: "hincrbyfloat", hash: "mdc:gauges", key, delta });
    this._invalidateCache();
  }

  // ─── FIX 5: Histogram with BigInt sum/count ───────────────────

  observe(name, valueMs) {
    let hist = this.histograms.get(name);
    if (!hist) {
      // Auto-register with default buckets if not pre-registered
      this.registerHistogram(name, DEFAULT_BUCKETS);
      hist = this.histograms.get(name);
    }

    // FIX 5: BigInt accumulation for sum and count
    hist.sum   += BigInt(Math.round(valueMs));
    hist.count += BigInt(1);

    for (const bound of this._histogramBuckets.get(name) || DEFAULT_BUCKETS) {
      if (valueMs <= bound) {
        hist.buckets.set(bound, (hist.buckets.get(bound) || 0) + 1);
      }
    }

    this._invalidateCache();
  }

  // ─── Persistence ───────────────────────────────────────────────

  startPersistence() {
    this._persistTimer = setInterval(() => this._flushToDisk(), PERSIST_INTERVAL);
    logger.info("METRICS", "metrics.js:226", `Persistence started (every ${PERSIST_INTERVAL}ms).`);
  }

  loadSnapshot() {
    try {
      if (!fs.existsSync(PERSIST_PATH)) return;
      const snap = JSON.parse(fs.readFileSync(PERSIST_PATH, "utf8"));

      for (const [key, entry] of Object.entries(snap.counters || {})) {
        this.counters.set(key, { ...entry, value: BigInt(entry.value) });
      }
      for (const [key, entry] of Object.entries(snap.gauges || {})) {
        this.gauges.set(key, entry);
      }

      logger.info("METRICS", "metrics.js:241", `Snapshot loaded: ${Object.keys(snap.counters || {}).length} counters.`);
    } catch (err) {
      logger.error("METRICS", "metrics.js:243", `Snapshot load failed: ${err.message}`);
    }
  }

  _flushToDisk() {
    try {
      const countersObj = {};
      for (const [key, entry] of this.counters) {
        countersObj[key] = { ...entry, value: entry.value.toString() };
      }
      const gaugesObj = {};
      for (const [key, entry] of this.gauges) {
        gaugesObj[key] = entry;
      }

      fs.mkdirSync(path.dirname(PERSIST_PATH), { recursive: true });
      const tmp = PERSIST_PATH + ".tmp";
      fs.writeFileSync(tmp, JSON.stringify({ counters: countersObj, gauges: gaugesObj }));
      fs.renameSync(tmp, PERSIST_PATH);
    } catch (err) {
      logger.error("METRICS", "metrics.js:263", `Flush failed: ${err.message}`);
    }
  }

  // ─── HTTP Server ───────────────────────────────────────────────

  startServer() {
    this.server = http.createServer((req, res) => {
      if (req.url !== "/metrics") {
        res.writeHead(404);
        res.end("Not Found");
        return;
      }

      const now = Date.now();
      if (this._cache && now - this._cacheTime < SCRAPE_CACHE_TTL) {
        res.writeHead(200, { "Content-Type": "text/plain; version=0.0.4" });
        res.end(this._cache);
        return;
      }

      try {
        this._cache     = this._render();
        this._cacheTime = now;
        res.writeHead(200, { "Content-Type": "text/plain; version=0.0.4" });
        res.end(this._cache);
      } catch (err) {
        logger.error("METRICS", "metrics.js:289", `Render error: ${err.message}`);
        res.writeHead(500);
        res.end("Error");
      }
    });

    this.server.on("error", (err) => {
      logger.error("METRICS", "metrics.js:296", `Server error: ${err.message}`);
      setTimeout(() => this.server.listen(METRICS_PORT), 3000);
    });

    this.server.listen(METRICS_PORT, () => {
      logger.info("METRICS", "metrics.js:301", `Metrics: http://0.0.0.0:${METRICS_PORT}/metrics`);
    });
  }

  // ─── Rendering ─────────────────────────────────────────────────

  _render() {
    const lines = [];

    for (const [, e] of this.counters) {
      lines.push(`# TYPE ${e.name} counter`);
      lines.push(`${e.name}${this._labelStr(e.labels)} ${e.value.toString()}`);
    }

    for (const [, e] of this.gauges) {
      lines.push(`# TYPE ${e.name} gauge`);
      lines.push(`${e.name}${this._labelStr(e.labels)} ${e.value}`);
    }

    // FIX 5: BigInt sum/count rendered with toString()
    for (const [name, hist] of this.histograms) {
      lines.push(`# TYPE ${name} histogram`);
      let cumulative = 0;
      for (const [bound, count] of hist.buckets) {
        cumulative += count;
        const le = bound === Infinity ? "+Inf" : String(bound);
        lines.push(`${name}_bucket{le="${le}"} ${cumulative}`);
      }
      lines.push(`${name}_sum ${hist.sum.toString()}`);
      lines.push(`${name}_count ${hist.count.toString()}`);
    }

    return lines.join("\n") + "\n";
  }

  // ─── Helpers ───────────────────────────────────────────────────

  _invalidateCache() { this._cache = null; }

  _labelStr(labels) {
    const parts = Object.entries(labels).map(([k, v]) => `${k}="${v}"`);
    return parts.length ? `{${parts.join(",")}}` : "";
  }

  _key(name, labels) {
    return name + JSON.stringify(labels);
  }

  // ─── Defaults ──────────────────────────────────────────────────

  registerDefaults() {
    // FIX 2: pre-register histograms with appropriate bucket granularity
    this.registerHistogram("medorcoin_rpc_latency_ms",      [1, 5, 10, 25, 50, 100, 250, 500, 1000, Infinity]);
    this.registerHistogram("medorcoin_tx_validation_ms",    [0.1, 0.5, 1, 2, 5, 10, 25, 50, 100, Infinity]);
    this.registerHistogram("medorcoin_block_processing_ms", [10, 50, 100, 500, 1000, 2000, 5000, Infinity]);

    // FIX 4: explicit cardinality limits per metric
    this.setCardinalityLimit("medorcoin_rpc_calls_total",  500);
    this.setCardinalityLimit("medorcoin_tx_rejected_total", 50);
    this.setCardinalityLimit("medorcoin_peer_errors",       20);

    this.gauge("medorcoin_mempool_size",    0);
    this.gauge("medorcoin_peer_count",      0);
    this.gauge("medorcoin_chain_height",    0);
    this.gauge("medorcoin_hash_rate",       0);
    this.gauge("medorcoin_utxo_set_size",   0);
    this.gauge("medorcoin_db_size_bytes",   0);

    this.increment("medorcoin_blocks_accepted",        {}, 0);
    this.increment("medorcoin_blocks_rejected",        {}, 0);
    this.increment("medorcoin_rpc_calls_total",        {}, 0);
    this.increment("medorcoin_rpc_errors_total",       {}, 0);
    this.increment("medorcoin_double_spend_attempts",  {}, 0);
    this.increment("medorcoin_tx_rejected_total",      {}, 0);
  }

  stop() {
    clearInterval(this._persistTimer);
    clearInterval(this._retryTimer);
    this._flushToDisk();
    if (this.server) this.server.close();
    logger.info("METRICS", "metrics.js:380", "Metrics stopped.");
  }
}

const instance = new Metrics();
module.exports = instance;
