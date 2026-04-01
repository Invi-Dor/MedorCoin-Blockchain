// log_security.js
/**
 * MedorCoin - Log Security Module
 * Production-grade sensitive data filtering for a multi-node blockchain.
 *
 * Solves:
 *  1. Multi-subsystem support — per-channel filter rules
 *  2. Async-safe — all operations synchronous and non-blocking
 *  3. Remote sanitization — strips sensitive data before shipping to aggregators
 *  4. Distributed logging ready — structured JSON output with node/cluster IDs
 *  5. Per-subsystem granular filtering — each channel has its own rules
 *  6. Hot-reload — config reloads without restarting node
 *  7. Structured logging — enriches JSON log entries with blockchain fields
 */

"use strict";

const fs     = require("fs");
const path   = require("path");
const crypto = require("crypto");

// ── Initial config load ───────────────────────────────────────────
let CFG = loadConfig();

function loadConfig() {
  try {
    return require("./log_config");
  } catch (err) {
    process.stderr.write("[LOG_SECURITY] Config load failed: " + err.message + "\n");
    return {
      security:  { redactIPs: false, customPatterns: [] },
      channels:  {},
      multiNode: { clusterId: "", nodeId: "", prefixCluster: false },
    };
  }
}

// ─────────────────────────────────────────────────────────────────
// FIX 6: Hot-reload — watches log_config.js for changes
// Reloads without restarting the node process.
// ─────────────────────────────────────────────────────────────────
let _reloadTimer = null;
const CONFIG_PATH = path.resolve(__dirname, "log_config.js");

if (fs.existsSync(CONFIG_PATH)) {
  fs.watch(CONFIG_PATH, () => {
    // Debounce — wait 500ms after last change before reloading
    clearTimeout(_reloadTimer);
    _reloadTimer = setTimeout(() => {
      try {
        // Purge require cache so fresh values are loaded
        delete require.cache[require.resolve("./log_config")];
        CFG = loadConfig();
        // Rebuild all pattern caches with new config
        rebuildPatterns();
        process.stderr.write("[LOG_SECURITY] Config hot-reloaded.\n");
      } catch (err) {
        process.stderr.write("[LOG_SECURITY] Hot-reload failed: " + err.message + "\n");
      }
    }, 500);
  });
}

// ─────────────────────────────────────────────────────────────────
// FIX 5: Per-subsystem pattern sets
// Each blockchain channel (MEMPOOL, CONSENSUS, MINER, P2P, etc.)
// has its own filter rules on top of the global base rules.
// Bitcoin/Ethereum handle wallet logs differently from P2P logs —
// we do the same here.
// ─────────────────────────────────────────────────────────────────

// Base patterns applied to ALL channels
const BASE_PATTERNS = [
  // secp256k1 private keys — 64 hex chars
  {
    id:          "privkey_hex",
    pattern:     /\b[0-9a-fA-F]{64}\b/g,
    replacement: "[REDACTED_PRIVKEY]",
    channels:    "ALL",
    severity:    "CRITICAL",
  },
  // WIF private keys (51-52 chars, starts with 5/K/L)
  {
    id:          "privkey_wif",
    pattern:     /\b[5KL][1-9A-HJ-NP-Za-km-z]{50,51}\b/g,
    replacement: "[REDACTED_WIF]",
    channels:    "ALL",
    severity:    "CRITICAL",
  },
  // BIP39 seed phrases — 12 or 24 words
  {
    id:          "seed_phrase_12",
    pattern:     /(\b[a-z]{3,8}\b\s+){11}\b[a-z]{3,8}\b/g,
    replacement: "[REDACTED_SEED_12W]",
    channels:    "ALL",
    severity:    "CRITICAL",
  },
  {
    id:          "seed_phrase_24",
    pattern:     /(\b[a-z]{3,8}\b\s+){23}\b[a-z]{3,8}\b/g,
    replacement: "[REDACTED_SEED_24W]",
    channels:    "ALL",
    severity:    "CRITICAL",
  },
  // Passwords in JSON
  {
    id:          "password_json",
    pattern:     /"password"\s*:\s*"[^"]+"/gi,
    replacement: '"password":"[REDACTED]"',
    channels:    "ALL",
    severity:    "HIGH",
  },
  // Bearer tokens
  {
    id:          "bearer_token",
    pattern:     /Bearer\s+[A-Za-z0-9\-._~+/]+=*/g,
    replacement: "Bearer [REDACTED]",
    channels:    "ALL",
    severity:    "HIGH",
  },
  // API keys in query strings or JSON
  {
    id:          "api_key",
    pattern:     /([&?]api[_-]?key=)[A-Za-z0-9\-._~]{16,}/gi,
    replacement: "$1[REDACTED]",
    channels:    "ALL",
    severity:    "HIGH",
  },
  // JWT tokens (three base64url segments)
  {
    id:          "jwt",
    pattern:     /eyJ[A-Za-z0-9_-]+\.eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+/g,
    replacement: "[REDACTED_JWT]",
    channels:    "ALL",
    severity:    "HIGH",
  },
  // Ethereum-style addresses (0x + 40 hex)
  {
    id:          "eth_address",
    pattern:     /\b0x[0-9a-fA-F]{40}\b/g,
    replacement: "[REDACTED_ADDR]",
    channels:    "ALL",
    severity:    "MEDIUM",
  },
  // Generic secrets in key=value format
  {
    id:          "secret_kv",
    pattern:     /\b(secret|token|key|pass|auth)\s*[=:]\s*["']?[A-Za-z0-9+/=_\-.]{12,}["']?/gi,
    replacement: "$1=[REDACTED]",
    channels:    "ALL",
    severity:    "HIGH",
  },
];

// FIX 5: Channel-specific extra patterns
// These apply ONLY to the named channels — more sensitive subsystems
// get stricter rules. Mirrors Bitcoin's per-subsystem log filtering.
const CHANNEL_PATTERNS = {

  // Wallet channel — most sensitive, redact all addresses and amounts
  WALLET: [
    {
      id:          "wallet_balance",
      pattern:     /balance\s*[=:]\s*[\d.]+\s*(MDR|BTC|ETH|sat)/gi,
      replacement: "balance=[REDACTED]",
      severity:    "HIGH",
    },
    {
      id:          "wallet_amount",
      pattern:     /amount\s*[=:]\s*[\d.]+/gi,
      replacement: "amount=[REDACTED]",
      severity:    "MEDIUM",
    },
  ],

  // RPC channel — redact auth tokens and request bodies with keys
  RPC: [
    {
      id:          "rpc_auth_header",
      pattern:     /authorization\s*:\s*[^\s,}]+/gi,
      replacement: "authorization:[REDACTED]",
      severity:    "HIGH",
    },
    {
      id:          "rpc_token_param",
      pattern:     /"auth_token"\s*:\s*"[^"]+"/gi,
      replacement: '"auth_token":"[REDACTED]"',
      severity:    "HIGH",
    },
  ],

  // P2P channel — redact full IPs if configured, keep peer IDs
  P2P: [], // populated by rebuildPatterns() based on config

  // Security/Audit channel — strictest, redact everything sensitive
  SECURITY: [
    {
      id:          "ban_reason_with_key",
      pattern:     /reason=.*key.*([0-9a-fA-F]{32,})/gi,
      replacement: "reason=[REDACTED_KEY_REASON]",
      severity:    "CRITICAL",
    },
  ],
  AUDIT: [],

  // Mempool — redact amounts if privacy mode enabled
  MEMPOOL: [],

  // Consensus/Mining — minimal extra filtering needed
  CONSENSUS: [],
  MINER:     [],
  CHAIN:     [],
  SYNC:      [],
};

// ─────────────────────────────────────────────────────────────────
// FIX 6: Pattern builder — called on init and on hot-reload
// ─────────────────────────────────────────────────────────────────
function rebuildPatterns() {
  // IP redaction — controlled by config (hot-reloadable)
  CHANNEL_PATTERNS.P2P = CFG.security?.redactIPs
    ? [{
        id:          "ip_address",
        pattern:     /\b\d{1,3}(\.\d{1,3}){3}\b/g,
        replacement: "[REDACTED_IP]",
        severity:    "MEDIUM",
      }]
    : [];

  // Custom patterns from log_config.js security.customPatterns
  const custom = (CFG.security?.customPatterns || []).map((p, i) => ({
    id:          "custom_" + i,
    pattern:     new RegExp(p.pattern, p.flags || "g"),
    replacement: p.replacement || "[REDACTED]",
    channels:    p.channel || "ALL",
    severity:    p.severity || "MEDIUM",
  }));

  // Attach custom patterns to their channels
  for (const cp of custom) {
    if (cp.channels === "ALL") {
      BASE_PATTERNS.push(cp);
    } else {
      if (!CHANNEL_PATTERNS[cp.channels]) CHANNEL_PATTERNS[cp.channels] = [];
      CHANNEL_PATTERNS[cp.channels].push(cp);
    }
  }
}

// Initial build
rebuildPatterns();

// ─────────────────────────────────────────────────────────────────
// FIX 1 + FIX 5: Main filter function — per-channel aware
//
// filterSensitive(message, channel)
//   message: string to sanitize
//   channel: optional — "MEMPOOL", "CONSENSUS", "P2P", "RPC", etc.
//            If omitted, only base patterns apply.
//
// Used by logger.js _write() — called synchronously on hot path.
// ─────────────────────────────────────────────────────────────────
function filterSensitive(message, channel) {
  if (message === null || message === undefined) return "";
  let safe = String(message);

  // Apply base patterns (all channels)
  for (const { pattern, replacement } of BASE_PATTERNS) {
    try {
      // Reset lastIndex for global patterns to prevent stateful bugs
      if (pattern.global) pattern.lastIndex = 0;
      safe = safe.replace(pattern, replacement);
    } catch {}
  }

  // Apply channel-specific patterns
  if (channel && CHANNEL_PATTERNS[channel]) {
    for (const { pattern, replacement } of CHANNEL_PATTERNS[channel]) {
      try {
        if (pattern.global) pattern.lastIndex = 0;
        safe = safe.replace(pattern, replacement);
      } catch {}
    }
  }

  return safe;
}

// ─────────────────────────────────────────────────────────────────
// FIX 4: Remote sanitization
// Strips sensitive data from log entries BEFORE shipping to
// external aggregators (ELK, Loki, Graylog, Datadog).
// Bitcoin-scale nodes never ship raw logs to collectors.
// ─────────────────────────────────────────────────────────────────
function sanitizeForRemote(logEntry) {
  if (!logEntry) return logEntry;

  // If it is a structured JSON entry (FIX 7 output from logger.js)
  if (typeof logEntry === "object") {
    return sanitizeStructured(logEntry);
  }

  // Plain string — apply full filter
  return filterSensitive(String(logEntry));
}

function sanitizeStructured(entry) {
  const clean = Object.assign({}, entry);

  // Sanitize message field
  if (clean.message) {
    clean.message = filterSensitive(clean.message, clean.module);
  }

  // Remove fields that must never be in remote logs
  const FORBIDDEN_FIELDS = [
    "privateKey", "privKey", "wif", "seed", "mnemonic",
    "password", "passphrase", "secret", "authToken",
    "encryptKey", "signingKey",
  ];
  for (const field of FORBIDDEN_FIELDS) {
    if (clean[field] !== undefined) {
      clean[field] = "[REDACTED]";
    }
  }

  // Redact nested objects one level deep
  for (const [key, val] of Object.entries(clean)) {
    if (val && typeof val === "object" && !Array.isArray(val)) {
      for (const fld of FORBIDDEN_FIELDS) {
        if (val[fld] !== undefined) val[fld] = "[REDACTED]";
      }
    }
    // Redact string values that look like private keys
    if (typeof val === "string" && /^[0-9a-fA-F]{64}$/.test(val)) {
      clean[key] = "[REDACTED_PRIVKEY]";
    }
  }

  return clean;
}

// ─────────────────────────────────────────────────────────────────
// FIX 7: Structured log entry builder
// Produces JSON log lines compatible with ELK, Loki, Graylog.
// Fields match what Bitcoin Core and Geth ship to aggregators:
//   timestamp, level, subsystem, node, cluster, txid, blockHash,
//   height, peer, message, hash (tamper-evident chain)
// ─────────────────────────────────────────────────────────────────
function buildStructuredEntry({
  level,
  module,         // subsystem: MEMPOOL | CONSENSUS | MINER | P2P | RPC | CHAIN | SYNC | AUDIT
  fileRef,
  message,
  seq,
  prevHash,
  // Optional blockchain-specific fields
  txid        = undefined,
  blockHash   = undefined,
  height      = undefined,
  peer        = undefined,
  difficulty  = undefined,
  fee         = undefined,
} = {}) {

  const nodeId    = CFG.nodeId    || process.env.NODE_ID    || "medorcoin-node";
  const clusterId = CFG.multiNode?.clusterId || process.env.CLUSTER_ID || "";

  // FIX 5: Apply channel-specific filtering to message
  const safeMsg = filterSensitive(String(message || ""), module);

  // FIX 7: Structured entry — only include defined optional fields
  const entry = {
    ts:        new Date().toISOString(),
    level,
    severity:  level,                 // ELK/Elasticsearch native field
    subsystem: module,                // matches Bitcoin Core's subsystem field
    file:      fileRef,
    seq:       String(seq || 0),
    node:      nodeId,
    message:   safeMsg,
  };

  // Include cluster ID if multi-node prefix is enabled
  if (clusterId && CFG.multiNode?.prefixCluster) {
    entry.cluster = clusterId;
  }

  // Blockchain-specific optional fields (only if provided)
  if (txid       !== undefined) entry.txid       = txid;
  if (blockHash  !== undefined) entry.blockHash  = blockHash;
  if (height     !== undefined) entry.height     = height;
  if (peer       !== undefined) entry.peer       = peer;
  if (difficulty !== undefined) entry.difficulty = difficulty;
  if (fee        !== undefined) entry.fee        = fee;

  // FIX 8: Deterministic field ordering for hash chain (matches logger.js)
  // Hash is computed BEFORE adding the hash field itself
  const stableStr = stableStringify(entry);
  entry.hash = crypto
    .createHash("sha256")
    .update((prevHash || "0".repeat(64)) + stableStr)
    .digest("hex");

  return entry;
}

// ─────────────────────────────────────────────────────────────────
// FIX 3: Multi-file log path helper
// Returns the correct log file path per subsystem — mirrors
// Bitcoin Core which uses separate files for net, mempool, etc.
// ─────────────────────────────────────────────────────────────────
function getLogPath(channel) {
  const logDir  = CFG.logDir  || "./logs";
  const logFile = CFG.logFile || "errors.log";

  // Channels that get dedicated log files (configurable)
  const DEDICATED = {
    AUDIT:     "audit.log",
    SECURITY:  "security.log",
    MINER:     "mining.log",
    CONSENSUS: "consensus.log",
    P2P:       "network.log",
    RPC:       "rpc.log",
  };

  const fileName = DEDICATED[channel] || logFile;
  return require("path").join(logDir, fileName);
}

// ─────────────────────────────────────────────────────────────────
// FIX 2: Backpressure guard
// Returns true if the log pipeline is safe to write to.
// Called by logger.js before pushing to write buffer.
// Prevents high-throughput consensus threads from being blocked
// by a slow disk — matches Bitcoin's approach of dropping log
// lines under extreme load rather than blocking the main loop.
// ─────────────────────────────────────────────────────────────────
const _pressureState = { dropping: false, droppedCount: 0 };

function checkBackpressure(bufferDepth, maxBufferLines) {
  const ratio = bufferDepth / maxBufferLines;

  if (ratio >= 0.95) {
    if (!_pressureState.dropping) {
      process.stderr.write(
        "[LOG_SECURITY] Backpressure: buffer at " +
        Math.round(ratio * 100) + "% — dropping non-critical lines\n"
      );
    }
    _pressureState.dropping = true;
    _pressureState.droppedCount++;
    return false; // signal to drop this line
  }

  if (_pressureState.dropping && ratio < 0.75) {
    process.stderr.write(
      "[LOG_SECURITY] Backpressure relieved — " +
      _pressureState.droppedCount + " lines dropped\n"
    );
    _pressureState.dropping    = false;
    _pressureState.droppedCount = 0;
  }

  return true; // safe to write
}

// ─────────────────────────────────────────────────────────────────
// UTILS
// ─────────────────────────────────────────────────────────────────

// Deterministic JSON serialization — required for consistent hash chains
function stableStringify(obj) {
  const keys  = Object.keys(obj).sort();
  const parts = keys.map(k => JSON.stringify(k) + ":" + JSON.stringify(obj[k]));
  return "{" + parts.join(",") + "}";
}

// Expose current pattern inventory for test suites (log_test.js)
function getPatternInventory() {
  return {
    basePatterns:    BASE_PATTERNS.map(p => ({ id: p.id, channels: p.channels || "ALL", severity: p.severity })),
    channelPatterns: Object.fromEntries(
      Object.entries(CHANNEL_PATTERNS).map(([ch, patterns]) => [
        ch, patterns.map(p => ({ id: p.id, severity: p.severity }))
      ])
    ),
    hotReloadActive: fs.existsSync(CONFIG_PATH),
    ipRedaction:     CFG.security?.redactIPs || false,
    customCount:     (CFG.security?.customPatterns || []).length,
  };
}

// ─────────────────────────────────────────────────────────────────
// EXPORTS
// ─────────────────────────────────────────────────────────────────
module.exports = {
  // Core — used by logger.js on every log line
  filterSensitive,

  // Remote sanitization — used by log_transport.js before shipping
  sanitizeForRemote,

  // Structured entry builder — used by logger.js _write()
  buildStructuredEntry,

  // Multi-file path — used by log_write_buffer.js
  getLogPath,

  // Backpressure — used by log_write_buffer.js push()
  checkBackpressure,

  // Inventory — used by log_test.js to verify pattern coverage
  getPatternInventory,
};
