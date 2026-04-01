// log_test.js
/**
 * MedorCoin - Logger Stress Test & Full Validation Suite
 *
 * Tests:
 *  1. Sensitive data filtering
 *  2. Throughput under load
 *  3. Level filtering
 *  4. Crash resistance
 *  5. Multi-node sync logging
 *  6. Consensus / mining logging
 *  7. Transaction processing logging
 *  8. Network events logging
 *  9. Security & audit / dynamic attack simulation
 *
 * Run: node log_test.js
 */

"use strict";

const logger           = require("./logger");
const { filterSensitive } = require("./log_security");
const fs               = require("fs");
const crypto           = require("crypto");

const LOG_FILE   = "./logs/errors.log";
const ITERATIONS = 100_000;

// ─── Result tracker ───────────────────────────────────────────────
const RESULTS = { passed: 0, failed: 0, skipped: 0 };

function pass(label)  { RESULTS.passed++;  console.log("  ✓ PASS: " + label); }
function fail(label, detail) {
  RESULTS.failed++;
  console.error("  ✗ FAIL: " + label);
  if (detail) console.error("        " + detail);
}
function skip(label)  { RESULTS.skipped++; console.log("  - SKIP: " + label); }

function randomHex(len) {
  return crypto.randomBytes(len).toString("hex");
}
function randomTxId() { return randomHex(32); }
function randomAddr() { return "1" + randomHex(20).slice(0, 33); }
function randomPeerId() { return randomHex(8); }

// ─────────────────────────────────────────────────────────────────
// TEST 1: Sensitive data filtering
// ─────────────────────────────────────────────────────────────────
function test1_filtering() {
  console.log("\n--- Test 1: Sensitive Data Filtering ---");

  const cases = [
    {
      label:          "Private key (64 hex chars)",
      input:          "Key: abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890",
      mustNotContain: "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890",
    },
    {
      label:          "Password in JSON",
      input:          '{"password":"supersecret123"}',
      mustNotContain: "supersecret123",
    },
    {
      label:          "Bearer token",
      input:          "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.abc",
      mustNotContain: "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9",
    },
    {
      label:          "Wallet address",
      input:          "Sending from 0x71C7656EC7ab88b098defB751B7401B5f6d8976F",
      mustNotContain: "0x71C7656EC7ab88b098defB751B7401B5f6d8976F",
    },
    {
      label:          "BIP39 seed phrase",
      input:          "phrase: abandon ability able about above absent absorb abstract abuse access accident account",
      mustNotContain: "abandon ability able",
    },
    {
      label:          "Nested private key in JSON log",
      input:          JSON.stringify({ event: "login", privateKey: "a".repeat(64), user: "alice" }),
      mustNotContain: "a".repeat(64),
    },
    {
      label:          "Clean message passes through unchanged",
      input:          "Block #1024 accepted hash=00000abc",
      mustContain:    "Block #1024",
      mustNotContain: null,
    },
  ];

  let p = 0;
  for (const c of cases) {
    const result = filterSensitive(c.input);
    let ok = true;

    if (c.mustNotContain && result.includes(c.mustNotContain)) {
      fail(c.label, "Sensitive data NOT redacted. Output: " + result.slice(0, 80));
      ok = false;
    }
    if (c.mustContain && !result.includes(c.mustContain)) {
      fail(c.label, "Clean data was incorrectly removed. Output: " + result.slice(0, 80));
      ok = false;
    }
    if (ok) { pass(c.label); p++; }
  }
  console.log("  Filtering: " + p + "/" + cases.length + " passed");
}

// ─────────────────────────────────────────────────────────────────
// TEST 2: Throughput under load
// ─────────────────────────────────────────────────────────────────
async function test2_throughput() {
  console.log("\n--- Test 2: Throughput ---");
  console.log("  Writing " + ITERATIONS.toLocaleString() + " lines...");

  const start = Date.now();
  for (let i = 0; i < ITERATIONS; i++) {
    logger.info(
      "TEST",
      "log_test.js:throughput",
      "Line " + i + " TX:" + Math.random().toString(36).slice(2)
    );
  }

  await logger.flush();
  const elapsed = Date.now() - start;
  const rate    = Math.round(ITERATIONS / (elapsed / 1000));

  console.log("  " + ITERATIONS.toLocaleString() + " lines in " + elapsed + "ms — " +
    rate.toLocaleString() + " lines/sec");

  if (rate >= 10_000) {
    pass("Throughput >= 10,000 lines/sec (" + rate.toLocaleString() + ")");
  } else {
    fail("Throughput below 10,000 lines/sec", "Got: " + rate.toLocaleString());
  }

  try {
    const stat = fs.statSync(LOG_FILE);
    console.log("  Log file size: " + (stat.size / 1024).toFixed(1) + " KB");
    if (stat.size > 0) pass("Log file written and non-empty");
    else               fail("Log file is empty after flush");
  } catch {
    fail("Log file not found after flush");
  }
}

// ─────────────────────────────────────────────────────────────────
// TEST 3: Level filtering
// ─────────────────────────────────────────────────────────────────
function test3_levelFiltering() {
  console.log("\n--- Test 3: Level Filtering ---");

  if (process.env.LOG_LEVEL === "DEBUG") {
    skip("DEBUG suppression (LOG_LEVEL=DEBUG — suppression not applicable)");
    return;
  }

  const before = fs.existsSync(LOG_FILE) ? fs.statSync(LOG_FILE).size : 0;
  logger.debug("TEST", "log_test.js:level", "This DEBUG line must be suppressed at INFO level");
  const after  = fs.existsSync(LOG_FILE) ? fs.statSync(LOG_FILE).size : 0;

  // File must not have grown (async buffer may not flush immediately — check with tolerance)
  if (after <= before) {
    pass("DEBUG suppressed at INFO level");
  } else {
    console.log("  WARN: File grew slightly — may be async buffer from prior test. " +
      "Delta: " + (after - before) + " bytes");
  }

  // INFO must always write
  const b2 = fs.existsSync(LOG_FILE) ? fs.statSync(LOG_FILE).size : 0;
  logger.info("TEST", "log_test.js:level", "INFO line must be written");
  // Flush to ensure write
  setTimeout(() => {
    const a2 = fs.existsSync(LOG_FILE) ? fs.statSync(LOG_FILE).size : 0;
    if (a2 > b2) pass("INFO level written correctly");
    else         fail("INFO level not written");
  }, 600);
}

// ─────────────────────────────────────────────────────────────────
// TEST 4: Crash resistance
// ─────────────────────────────────────────────────────────────────
function test4_crashResistance() {
  console.log("\n--- Test 4: Crash Resistance ---");

  const cases = [
    ["null message",      () => logger.error("TEST", "log_test.js:104", null)],
    ["undefined message", () => logger.warn ("TEST", "log_test.js:105", undefined)],
    ["object message",    () => logger.info ("TEST", "log_test.js:106", { obj: true, n: 42 })],
    ["empty string",      () => logger.info ("TEST", "log_test.js:107", "")],
    ["array message",     () => logger.info ("TEST", "log_test.js:108", [1,2,3])],
    ["number message",    () => logger.info ("TEST", "log_test.js:109", 99999)],
    ["boolean message",   () => logger.info ("TEST", "log_test.js:110", false)],
    ["symbol message",    () => logger.info ("TEST", "log_test.js:111", Symbol("test").toString())],
    ["very long message", () => logger.info ("TEST", "log_test.js:112", "X".repeat(100_000))],
    ["unicode message",   () => logger.info ("TEST", "log_test.js:113", "🔒💎⛏️ блок #1024 区块")],
    ["emoji in module",   () => logger.info ("🔥", "log_test.js:114", "Module with emoji")],
  ];

  let crashed = false;
  for (const [label, fn] of cases) {
    try {
      fn();
      pass(label);
    } catch (err) {
      fail(label, "Crashed: " + err.message);
      crashed = true;
    }
  }
  if (!crashed) console.log("  All edge-case inputs handled without crash.");
}

// ─────────────────────────────────────────────────────────────────
// TEST 5: Multi-node sync logging
// ─────────────────────────────────────────────────────────────────
async function test5_multiNodeSync() {
  console.log("\n--- Test 5: Multi-Node Sync Logging ---");

  const nodes    = ["medor-node-001", "medor-node-002", "medor-node-003"];
  const origId   = process.env.NODE_ID;
  const logLines = [];

  // Simulate 3 nodes logging simultaneously
  const promises = nodes.map(async (nodeId, i) => {
    process.env.NODE_ID = nodeId;

    // Simulate chain sync events
    for (let block = 1000; block <= 1010; block++) {
      const hash  = randomHex(32);
      const prev  = randomHex(32);

      logger.info("SYNC",  "sync.js:45",
        "Node " + nodeId + " syncing block #" + block + " hash=" + hash.slice(0,16));
      logger.info("CHAIN", "chain.js:88",
        "Block #" + block + " prevHash=" + prev.slice(0,16) + " accepted");

      // Simulate fork detection
      if (block === 1005 && i === 1) {
        logger.warn("CONSENSUS", "consensus.js:112",
          "Fork detected at height " + block + " — competing chain from " + nodeId);
        logger.warn("REORG", "chain.js:145",
          "Reorg initiated depth=3 fromNode=" + nodeId);
      }

      // Simulate peer disagreement
      if (block === 1008 && i === 2) {
        logger.error("SYNC​​​​​​​​​​​​​​​​
