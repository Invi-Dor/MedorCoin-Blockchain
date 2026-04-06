// log_config.js
/**
 * MedorCoin - Logger Configuration
 * Single source of truth for all logging behavior.
 * Override any value with environment variables.
 */

module.exports = {
  // Core
  nodeId:         process.env.NODE_ID        || "medorcoin-node-1",
  defaultLevel:   process.env.LOG_LEVEL      || "INFO",    // DEBUG | INFO | WARN | ERROR
  logDir:         process.env.LOG_DIR        || "./logs",
  logFile:        process.env.LOG_FILE       || "errors.log",

  // Write buffer
  flushIntervalMs: parseInt(process.env.LOG_FLUSH_MS   || "500"),
  maxBufferLines:  parseInt(process.env.LOG_BUFFER_MAX || "2000"),

  // Rotation
  rotation: {
    maxSizeBytes: parseInt(process.env.LOG_MAX_SIZE  || String(10 * 1024 * 1024)), // 10MB
    maxFiles:     parseInt(process.env.LOG_MAX_FILES || "10"),
    compress:     process.env.LOG_COMPRESS !== "false",  // default true
  },

  // Remote transport (optional — leave url empty to disable)
  transport: {
    url:             process.env.LOG_TRANSPORT_URL     || "",
    apiKey:          process.env.LOG_TRANSPORT_APIKEY  || "",
    timeoutMs:       parseInt(process.env.LOG_TRANSPORT_TIMEOUT || "3000"),
    batchSize:       parseInt(process.env.LOG_TRANSPORT_BATCH   || "50"),
    flushIntervalMs: parseInt(process.env.LOG_TRANSPORT_FLUSH   || "5001"),
  },

  // Security
  security: {
    redactIPs:      process.env.LOG_REDACT_IPS === "true",  // default false
    customPatterns: [],  // add { pattern, flags, replacement } objects here
  },

  // ── Multi-node simulation ──────────────────────────────────────
  // Controls how this node identifies itself and coordinates logging
  // across a cluster. Works with logger.js (nodeId per entry) and
  // log_test.js (NODE_ID env swapped per simulated node).
  multiNode: {
    // Unique cluster name — appears in every log entry when set.
    // Set via: CLUSTER_ID=mainnet-cluster-1 node node.js
    clusterId: process.env.CLUSTER_ID || "",

    // Maximum number of nodes this config supports for log correlation.
    // log_test.js uses this to know how many node IDs to simulate.
    maxNodes: parseInt(process.env.LOG_MAX_NODES || "10"),

    // If true, logger.js prefixes every line with clusterId+nodeId
    // so central aggregators (ELK, Grafana Loki) can filter per node.
    prefixCluster: process.env.LOG_PREFIX_CLUSTER === "true",
  },

  // ── Blockchain-specific log channels ──────────────────────────
  // Each channel maps to a module name used in logger calls:
  //   logger.info("MEMPOOL", ...) logger.warn("CONSENSUS", ...)
  // These settings let operators enable verbose logging per channel
  // without raising the global LOG_LEVEL to DEBUG.
  //
  // Works with log_test.js tests 5–9 which emit logs on these channels.
  // Works with logger.js _write() which checks channel overrides before
  // applying the global MIN_LEVEL filter.
  channels: {
    // Mempool: TX acceptance, rejection, eviction, double-spend
    MEMPOOL:   process.env.LOG_CHANNEL_MEMPOOL    || "INFO",

    // Consensus: block validation, PoW check, difficulty, halving
    CONSENSUS: process.env.LOG_CHANNEL_CONSENSUS  || "INFO",

    // Mining: nonce attempts, block found, coinbase, hash rate
    MINER:     process.env.LOG_CHANNEL_MINER      || "INFO",

    // Chain: reorgs, tip changes, UTXO apply, genesis verification
    CHAIN:     process.env.LOG_CHANNEL_CHAIN      || "INFO",

    // P2P: peer connect/disconnect, relay, ban, bootnode
    P2P:       process.env.LOG_CHANNEL_P2P        || "INFO",

    // Sync: block sync, fork detection, peer disagreement
    SYNC:      process.env.LOG_CHANNEL_SYNC       || "INFO",

    // RPC: request handling, auth, rate limit, error codes
    RPC:       process.env.LOG_CHANNEL_RPC        || "WARN",

    // Security/Audit: bans, attacks, key rotation, audit trail
    SECURITY:  process.env.LOG_CHANNEL_SECURITY   || "WARN",
    AUDIT:     process.env.LOG_CHANNEL_AUDIT      || "INFO",

    // Reorg: chain reorganization events
    REORG:     process.env.LOG_CHANNEL_REORG      || "WARN",
  },
};
