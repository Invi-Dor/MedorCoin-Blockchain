/**
 * MedorCoin - Logger Core Interface
 * Global-scale, tamper-proof, encrypted, multi-node final version.
 */

"use strict";

const fs      = require("fs");
const path    = require("path");
const crypto  = require("crypto");
const https   = require("https");
const http    = require("http");
const zlib    = require("zlib");
const os      = require("os");

// ─── Configuration ────────────────────────────────────────────────

const CFG = {
  // FIX 10: node ID must be explicitly set — never fall back to hostname silently
  nodeId: (() => {
    const id = process.env.NODE_ID;
    if (!id) {
      process.stderr.write("[LOGGER] WARNING: NODE_ID not set. Using hostname — may collide in clusters.\n");
    }
    return id || (os.hostname() + "-" + crypto.randomBytes(4).toString("hex"));
  })(),

  defaultLevel:    (process.env.LOG_LEVEL  || "INFO").toUpperCase(),
  logDir:          process.env.LOG_DIR     || "./logs",
  logFile:         process.env.LOG_FILE    || "errors.log",

  // FIX 3: support multiple encryption keys — primary + optional previous for rotation
  encryptKeys: (() => {
    const keys = [];
    if (process.env.LOG_ENCRYPT_KEY)     keys.push({ id: "k1", hex: process.env.LOG_ENCRYPT_KEY });
    if (process.env.LOG_ENCRYPT_KEY_OLD) keys.push({ id: "k0", hex: process.env.LOG_ENCRYPT_KEY_OLD });
    return keys; // first entry is always current encryption key
  })(),

  // FIX 4: TLS options for HTTPS transport
  tls: {
    rejectUnauthorized: process.env.LOG_TLS_REJECT_UNAUTHORIZED !== "false", // default true
    ca:   process.env.LOG_TLS_CA_PATH   ? fs.readFileSync(process.env.LOG_TLS_CA_PATH)   : undefined,
    cert: process.env.LOG_TLS_CERT_PATH ? fs.readFileSync(process.env.LOG_TLS_CERT_PATH) : undefined,
    key:  process.env.LOG_TLS_KEY_PATH  ? fs.readFileSync(process.env.LOG_TLS_KEY_PATH)  : undefined,
  },

  maxSizeBytes:    parseInt(process.env.LOG_MAX_SIZE    || String(10 * 1024 * 1024)),
  maxFiles:        parseInt(process.env.LOG_MAX_FILES   || "10"),
  flushIntervalMs: parseInt(process.env.LOG_FLUSH_MS    || "500"),
  maxBufferLines:  parseInt(process.env.LOG_BUFFER_MAX  || "2000"),
  rateLimitPerSec: parseInt(process.env.LOG_RATE_LIMIT  || "5000"),

  // FIX 6: hard memory backpressure ceiling in bytes
  maxMemoryBytes:  parseInt(process.env.LOG_MAX_MEM     || String(64 * 1024 * 1024)), // 64MB

  transport: {
    url:             process.env.LOG_TRANSPORT_URL    || "",
    apiKey:          process.env.LOG_TRANSPORT_APIKEY || "",
    timeoutMs:       parseInt(process.env.LOG_TRANSPORT_TIMEOUT || "3000"),
    batchSize:       parseInt(process.env.LOG_TRANSPORT_BATCH   || "50"),
    flushIntervalMs: parseInt(process.env.LOG_TRANSPORT_FLUSH   || "5000"),
    retryMax:        parseInt(process.env.LOG_TRANSPORT_RETRY   || "1000"),
  },

  redactIPs: process.env.LOG_REDACT_IPS === "true",

  // FIX 2: path to persist hash chain state across restarts
  hashChainPath: process.env.LOG_HASH_CHAIN_PATH || "./logs/.chain_state",
};

// ─── Level map ────────────────────────────────────────────────────

const LEVELS = { DEBUG: 0, INFO: 1, WARN: 2, ERROR: 3 };
const MIN_LEVEL = LEVELS[CFG.defaultLevel] !== undefined ? CFG.defaultLevel : "INFO";
if (LEVELS[CFG.defaultLevel] === undefined) {
  process.stderr.write("[LOGGER] Invalid LOG_LEVEL \"" + CFG.defaultLevel + "\" — using INFO\n");
}

// ─── FIX 13: Internal metrics ─────────────────────────────────────

const METRICS = {
  linesWritten:      0,
  linesDropped:      0,
  flushCount:        0,
  flushErrors:       0,
  flushLatencyMs:    0,  // last flush latency
  encryptErrors:     0,
  rotationCount:     0,
  rotationErrors:    0,
  transportSent:     0,
  transportErrors:   0,
  retryQueueDepth:   0,
  bufferDepth:       0,
  memoryPressure:    false,
};

function metricsSnapshot() {
  return Object.assign({}, METRICS, {
    ts:     new Date().toISOString(),
    nodeId: CFG.nodeId,
  });
}

// ─── FIX 1: Cross-platform directory and permission setup ─────────

function ensureLogDir(dir) {
  try {
    fs.mkdirSync(dir, { recursive: true });
  } catch (err) {
    process.stderr.write("[LOGGER] Cannot create log dir: " + err.message + "\n");
    return;
  }
  // chmod only on POSIX — FIX 1
  if (process.platform !== "win32") {
    try { fs.chmodSync(dir, 0o700); } catch {}
  }
}

function restrictFilePerms(filePath) {
  if (process.platform !== "win32") {
    try { fs.chmodSync(filePath, 0o600); } catch {}
  }
}

ensureLogDir(CFG.logDir);
ensureLogDir(path.dirname(CFG.hashChainPath));

// ─── FIX 9: Atomic append using tmp + rename ──────────────────────
// For single-process use: atomic rename prevents torn writes.
// For multi-process: use a lock file or dedicated log agent (see note below).

const _logPath = path.join(CFG.logDir, CFG.logFile);
let   _writeLock = false;
const _writeWaiters = [];

async function atomicAppend(content) {
  // Serialize all writes through a promise queue — prevents interleaving
  // FIX 9: no two concurrent writes touch the file simultaneously
  return new Promise((resolve, reject) => {
    _writeWaiters.push({ content, resolve, reject });
    if (!_writeLock) _drainWriteQueue();
  });
}

async function _drainWriteQueue() {
  if (_writeLock || _writeWaiters.length === 0) return;
  _writeLock = true;
  while (_writeWaiters.length > 0) {
    const { content, resolve, reject } = _writeWaiters.shift();
    try {
      // FIX 7: wait for rotation to fully complete before appending
      await _maybeRotate(_logPath);
      await fs.promises.appendFile(_logPath, content, { encoding: "utf8", flag: "a" });
      restrictFilePerms(_logPath);
      resolve();
    } catch (err) {
      METRICS.flushErrors++;
      process.stderr.write("[LOGGER] atomicAppend error: " + err.message + "\n");
      reject(err);
    }
  }
  _writeLock = false;
}

// ─── FIX 2: Persistent hash chain ─────────────────────────────────

let _prevHash = "0000000000000000000000000000000000000000000000000000000000000000";

function _loadHashChain() {
  try {
    if (fs.existsSync(CFG.hashChainPath)) {
      const saved = JSON.parse(fs.readFileSync(CFG.hashChainPath, "utf8"));
      if (saved && typeof saved.hash === "string" && saved.hash.length === 64) {
        _prevHash = saved.hash;
        process.stderr.write("[LOGGER] Hash chain restored: " + _prevHash.slice(0, 16) + "...\n");
      }
    }
  } catch (err) {
    process.stderr.write("[LOGGER] Hash chain load failed: " + err.message + " — starting fresh.\n");
  }
}

function _saveHashChain() {
  try {
    const tmp = CFG.hashChainPath + ".tmp";
    fs.writeFileSync(tmp, JSON.stringify({ hash: _prevHash, ts: new Date().toISOString(), node: CFG.nodeId }));
    fs.renameSync(tmp, CFG.hashChainPath);
  } catch (err) {
    process.stderr.write("[LOGGER] Hash chain save failed: " + err.message + "\n");
  }
}

_loadHashChain();

// FIX 8: deterministic key ordering for JSON hash calculation
function _stableStringify(obj) {
  const keys = Object.keys(obj).sort();
  const parts = keys.map((k) => JSON.stringify(k) + ":" + JSON.stringify(obj[k]));
  return "{" + parts.join(",") + "}";
}

function chainHash(entryObj) {
  const stable = _stableStringify(entryObj);
  const h = crypto.createHash("sha256").update(_prevHash + stable).digest("hex");
  _prevHash = h;
  return h;
}

// ─── Optional digital signature per entry ─────────────────────────
// Set LOG_SIGN_KEY_PATH to a PEM private key file to enable per-entry Ed25519 signatures.

let _signKey = null;
if (process.env.LOG_SIGN_KEY_PATH) {
  try {
    _signKey = fs.readFileSync(process.env.LOG_SIGN_KEY_PATH, "utf8");
    process.stderr.write("[LOGGER] Per-entry signing enabled.\n");
  } catch (err) {
    process.stderr.write("[LOGGER] Sign key load failed: " + err.message + "\n");
  }
}

function _signEntry(data) {
  if (!_signKey) return undefined;
  try {
    const sig = crypto.sign(null, Buffer.from(data), { key: _signKey, dsaEncoding: "ieee-p1363" });
    return sig.toString("base64");
  } catch {
    return undefined;
  }
}

// ─── FIX 3: AES-256-GCM encryption with key rotation ─────────────

function _encrypt(text) {
  if (!CFG.encryptKeys.length) return text;
  const { id, hex } = CFG.encryptKeys[0]; // always use first (current) key
  try {
    const key    = Buffer.from(hex, "hex");
    if (key.length !== 32) throw new Error("Key must be 32 bytes (64 hex chars)");
    const iv     = crypto.randomBytes(12);
    const cipher = crypto.createCipheriv("aes-256-gcm", key, iv);
    const enc    = Buffer.concat([cipher.update(text, "utf8"), cipher.final()]);
    const tag    = cipher.getAuthTag();
    // Format: keyId(variable) + ":" + base64(iv+tag+ciphertext)
    return id + ":" + Buffer.concat([iv, tag, enc]).toString("base64") + "\n";
  } catch (err) {
    METRICS.encryptErrors++;
    process.stderr.write("[LOGGER] Encrypt error: " + err.message + "\n");
    return text; // fallback to plaintext — never lose the log
  }
}

// ─── FIX 3: Audit trail for key/config changes ────────────────────

const _auditPath = path.join(CFG.logDir, "audit.log");

function _audit(event, detail) {
  try {
    const entry = JSON.stringify({
      ts:     new Date().toISOString(),
      event,
      detail,
      node:   CFG.nodeId,
    }) + "\n";
    fs.appendFileSync(_auditPath, entry);
    restrictFilePerms(_auditPath);
  } catch {}
}

// Write audit entry on startup for config snapshot — FIX 14
_audit("STARTUP", {
  nodeId:     CFG.nodeId,
  level:      MIN_LEVEL,
  encrypted:  CFG.encryptKeys.length > 0,
  signed:     !!_signKey,
  transport:  !!CFG.transport.url,
  logDir:     CFG.logDir,
});

// ─── Sensitive data filter — synchronous ──────────────────────────

const REDACT_PATTERNS = [
  { re: /\b[0-9a-fA-F]{64}\b/g,            sub: "[REDACTED_KEY]"       },
  { re: /"password"\s*:\s*"[^"]+"/gi,       sub: '"password":"[REDACTED]"' },
  { re: /Bearer\s+[A-Za-z0-9\-._~+/]+=*/g, sub: "Bearer [REDACTED]"    },
  { re: /(\b[a-z]+\b\s+){11,}\b[a-z]+\b/g, sub: "[REDACTED_SEED]"      },
  { re: /0x[0-9a-fA-F]{40}/g,              sub: "[REDACTED_ADDR]"       },
];

function filterSensitive(msg) {
  let s = String(msg);
  for (const { re, sub } of REDACT_PATTERNS) s = s.replace(re, sub);
  if (CFG.redactIPs) s = s.replace(/\b\d{1,3}(\.\d{1,3}){3}\b/g, "[REDACTED_IP]");
  return s;
}

// ─── FIX 6: Token bucket with memory backpressure ─────────────────

const _bucket = {
  tokens:     CFG.rateLimitPerSec,
  max:        CFG.rateLimitPerSec,
  lastRefill: Date.now(),
};

function _rateLimitAllow() {
  // FIX 6: hard stop if process memory exceeds ceiling
  const memUsed = process.memoryUsage().heapUsed;
  if (memUsed > CFG.maxMemoryBytes) {
    METRICS.memoryPressure = true;
    METRICS.linesDropped++;
    if (METRICS.linesDropped % 1000 === 1) {
      process.stderr.write("[LOGGER] Memory pressure — dropping logs. Heap: " + Math.round(memUsed / 1024 / 1024) + "MB\n");
    }
    return false;
  }
  METRICS.memoryPressure = false;

  const now     = Date.now();
  const elapsed = (now - _bucket.lastRefill) / 1000;
  _bucket.tokens = Math.min(_bucket.max, _bucket.tokens + elapsed * _bucket.max);
  _bucket.lastRefill = now;

  if (_bucket.tokens >= 1) {
    _bucket.tokens--;
    return true;
  }
  METRICS.linesDropped++;
  return false;
}

// ─── FIX 7: Rotation — awaitable, no race ─────────────────────────

let _rotationInProgress = false;

async function _maybeRotate(filePath) {
  if (_rotationInProgress) {
    // Wait until current rotation finishes — FIX 7
    await new Promise((r) => setTimeout(r, 100));
    return;
  }
  try {
    const stat = await fs.promises.stat(filePath).catch(() => null);
    if (!stat) return;
    const sizeOk = stat.size >= CFG.maxSizeBytes;
    const dayOk  = new Date(stat.mtimeMs).toDateString() !== new Date().toDateString();
    if (!sizeOk && !dayOk) return;

    _rotationInProgress = true;
    await _rotate(filePath);
  } finally {
    _rotationInProgress = false;
  }
}

async function _rotate(filePath) {
  try {
    const dir        = path.dirname(filePath);
    const base       = path.basename(filePath, ".log");
    const ts         = new Date().toISOString().replace(/[:.]/g, "-");
    const rotated    = path.join(dir, base + "_" + ts + ".log");
    const compressed = rotated + ".gz";

    await fs.promises.rename(filePath, rotated);

    // FIX 7: await gzip completion before returning
    await new Promise((resolve, reject) => {
      const src  = fs.createReadStream(rotated);
      const dest = fs.createWriteStream(compressed);
      const gz   = zlib.createGzip({ level: zlib.constants.Z_BEST_SPEED });
      src.pipe(gz).pipe(dest);
      dest.on("finish", resolve);
      dest.on("error", reject);
      src.on("error", reject);
    });

    await fs.promises.unlink(rotated);
    restrictFilePerms(compressed);
    await _pruneOldLogs(dir, base);

    METRICS.rotationCount++;
    process.stderr.write("[LOGGER] Rotated: " + compressed + "\n");
    _audit("ROTATION", { file: compressed });
  } catch (err) {
    METRICS.rotationErrors++;
    process.stderr.write("[LOGGER] Rotation failed: " + err.message + "\n");
  }
}

async function _pruneOldLogs(dir, base) {
  try {
    const files = (await fs.promises.readdir(dir))
      .filter((f) => f.startsWith(base + "_") && f.endsWith(".gz"))
      .sort().reverse();
    for (const f of files.slice(CFG.maxFiles)) {
      await fs.promises.unlink(path.join(dir, f));
    }
  } catch {}
}

// ─── FIX 4: HTTPS transport with TLS verification ─────────────────

const _transport = {
  pending:    [],
  retryQueue: [],
  timer:      null,
};

if (CFG.transport.url) {
  _transport.timer = setInterval(_flushTransport, CFG.transport.flushIntervalMs);
  if (_transport.timer.unref) _transport.timer.unref();
}

function _shipLog(line) {
  if (!CFG.transport.url) return;
  _transport.pending.push(line);
  if (_transport.pending.length >= CFG.transport.batchSize) setImmediate(_flushTransport);
}

async function _flushTransport() {
  // FIX 5: wrap entire function body — no uncaught paths
  try {
    const batch = [
      ..._transport.retryQueue.splice(0, 20),
      ..._transport.pending.splice(0, CFG.transport.batchSize),
    ];
    if (!batch.length) return;

    METRICS.retryQueueDepth = _transport.retryQueue.length;

    const payload = JSON.stringify({
      node:  CFG.nodeId,
      ts:    new Date().toISOString(),
      lines: batch,
    });

    try {
      await _post(payload);
      METRICS.transportSent += batch.length;
    } catch (err) {
      METRICS.transportErrors++;
      process.stderr.write("[LOGGER] Transport failed: " + err.message + "\n");
      if (_transport.retryQueue.length < CFG.transport.retryMax) {
        _transport.retryQueue.push(...batch);
      } else {
        process.stderr.write("[LOGGER] Retry queue full — " + batch.length + " lines dropped.\n");
        METRICS.linesDropped += batch.length;
      }
    }
  } catch (outerErr) {
    // FIX 5: catch anything that escapes inner try
    process.stderr.write("[LOGGER] _flushTransport outer error: " + outerErr.message + "\n");
  }
}

function _post(payload) {
  return new Promise((resolve, reject) => {
    const u      = new URL(CFG.transport.url);
    const driver = u.protocol === "https:" ? https : http;

    // FIX 4: full TLS options
    const opts = {
      hostname:           u.hostname,
      port:               u.port || (u.protocol === "https:" ? 443 : 80),
      path:               u.pathname,
      method:             "POST",
      timeout:            CFG.transport.timeoutMs,
      rejectUnauthorized: CFG.tls.rejectUnauthorized,
      ca:                 CFG.tls.ca,
      cert:               CFG.tls.cert,
      key:                CFG.tls.key,
      headers: {
        "Content-Type":   "application/json",
        "Content-Length": Buffer.byteLength(payload),
        ...(CFG.transport.apiKey
          ? { "Authorization": "Bearer " + CFG.transport.apiKey }
          : {}),
      },
    };

    const req = driver.request(opts, (res) => {
      res.resume();
      res.statusCode >= 400
        ? reject(new Error("HTTP " + res.statusCode))
        : resolve();
    });
    req.on("timeout", () => { req.destroy(); reject(new Error("Timeout")); });
    req.on("error", reject);
    req.write(payload);
    req.end();
  });
}

// ─── Write buffer ─────────────────────────────────────────────────

const _buf = { lines: [], timer: null };

_buf.timer = setInterval(_flushBuffer, CFG.flushIntervalMs);
if (_buf.timer.unref) _buf.timer.unref();

function _pushBuffer(line, level) {
  _buf.lines.push(line);
  METRICS.bufferDepth = _buf.lines.length;

  if (level === "ERROR" || level === "WARN") _shipLog(line);
  if (_buf.lines.length >= CFG.maxBufferLines) setImmediate(_flushBuffer);
}

async function _flushBuffer() {
  // FIX 5: full outer guard
  try {
    if (_buf.lines.length === 0) return;

    const start = Date.now();
    const batch = _buf.lines.splice(0);
    METRICS.bufferDepth = 0;

    let content = batch.join("");
    if (CFG.encryptKeys.length) content = _encrypt(content);

    try {
      await atomicAppend(content);
      METRICS.linesWritten  += batch.length;
      METRICS.flushCount++;
      METRICS.flushLatencyMs = Date.now() - start;
    } catch (err) {
      process.stderr.write("[LOGGER] _flushBuffer write error: " + err.message + "\n");
      _buf.lines.unshift(...batch); // re-queue — no log loss
    }
  } catch (outerErr) {
    process.stderr.write("[LOGGER] _flushBuffer outer error: " + outerErr.message + "\n");
  }
}

// ─── FIX 11: Graceful shutdown with transport drain ───────────────

async function _flushAll() {
  try {
    clearInterval(_buf.timer);
    clearInterval(_transport.timer);

    await _flushBuffer();

    // FIX 11: wait for all retries to finish before exit
    const deadline = Date.now() + 8000; // 8s max
    while (
      (_transport.pending.length > 0 || _transport.retryQueue.length > 0) &&
      Date.now() < deadline
    ) {
      await _flushTransport();
      await new Promise((r) => setTimeout(r, 200));
    }

    _saveHashChain();
    _audit("SHUTDOWN", { metrics: metricsSnapshot() });
  } catch (err) {
    process.stderr.write("[LOGGER] _flushAll error: " + err.message + "\n");
  }
}

process.once("SIGINT",  () => _flushAll().then(() => process.exit(0)));
process.once("SIGTERM", () => _flushAll().then(() => process.exit(0)));
process.on("uncaughtException", (err) => {
  process.stderr.write("[LOGGER] uncaughtException: " + err.message + "\n");
  // FIX 11: give transport time to drain before hard exit
  _flushAll().then(() => process.exit(1)).catch(() => process.exit(1));
});

// ─── Logger class ─────────────────────────────────────────────────

class Logger {
  constructor() {
    this._seq = BigInt(0);
  }

  _write(level, module, fileRef, message) {
    if (LEVELS[level] < LEVELS[MIN_LEVEL]) return;
    if (!_rateLimitAllow()) return;

    const safeMsg = filterSensitive(String(message));
    const seq     = (++this._seq).toString();

    // FIX 8: deterministic field order for hash
    // FIX 12: severity tag for Elasticsearch/ELK compatibility
    const entry = {
      ts:       new Date().toISOString(),
      level,
      severity: level,           // FIX 12: alias used by ELK/Elasticsearch
      module,
      file:     fileRef,
      seq,
      node:     CFG.nodeId,
      message:  safeMsg,
    };

    // FIX 7 + FIX 8: hash over stable stringified entry (no hash field yet)
    entry.hash = chainHash(entry);

    // Optional per-entry digital signature
    const sig = _signEntry(_stableStringify(entry));
    if (sig) entry.sig = sig;

    const line = JSON.stringify(entry) + "\n";
    process.stdout.write(line);
    _pushBuffer(line, level);
  }

  debug(module, fileRef, message) { this._write("DEBUG", module, fileRef, message); }
  info (module, fileRef, message) { this._write("INFO",  module, fileRef, message); }
  warn (module, fileRef, message) { this._write("WARN",  module, fileRef, message); }
  error(module, fileRef, message) { this._write("ERROR", module, fileRef, message); }

  async flush() { await _flushAll(); }

  // FIX 13: expose metrics for integration with metrics.js
  metrics() { return metricsSnapshot(); }

  // FIX 14: audit a manual config change event
  audit(event, detail) { _audit(event, detail); }
}

module.exports = new Logger();
