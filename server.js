// Medor-Blockchain/server.js
// Production glue: REST API + addon bridge

const express = require("express");
const bodyParser = require("body-parser");
const http = require("http");
const WebSocket = require("ws");
const path = require("path");
require("dotenv").config();

// Load the addon (adjust the path if you place the build output elsewhere)
let addon;
try {
  // If you use node-gyp, the default path is build/Release/medorcoin_addon.node
  addon = require(path.join(__dirname, "build", "Release", "medorcoin_addon.node"));
} catch (e) {
  // Fallback for some environments
  try {
    addon = require(path.join(__dirname, "build", "Release", "medorcoin_addon"));
  } catch (err) {
    console.error("Failed to load medorcoin_addon:", err);
    addon = null;
  }
}

const app = express();
const server = http.createServer(app);
const PORT = process.env.PORT || 5000;

app.use(bodyParser.json());

// Basic health
app.get("/health", (req, res) => res.json({ status: "ok", time: Date.now() }));

// If addon is not loaded, fail fast for all core routes
function withAddon(res, cb) {
  if (!addon) {
    res.status(503).json({ error: "core_unavailable" });
    return;
  }
  try {
    return cb();
  } catch (err) {
    console.error("Addon error:", err);
    res.status(500).json({ error: "internal_error", detail: err.message });
  }
}

// GET /balance?address=...
app.get("/balance", (req, res) => {
  withAddon(res, () => {
    const addr = req.query.address;
    if (!addr) return res.status(400).json({ error: "missing address" });
    const bal = addon.getBalance(addr);
    res.json({ address: addr, balance: bal });
  });
});

// GET /utxos?address=...
app.get("/utxos", (req, res) => {
  withAddon(res, () => {
    const addr = req.query.address;
    if (!addr) return res.status(400).json({ error: "missing address" });
    // If the addon exposes a getUTXOs method, wire it here. Otherwise, use your existing data store.
    const utxos = addon.getUTXOs ? addon.getUTXOs(addr) : [];
    res.json({ address: addr, utxos });
  });
});

// POST /tx
// Body: { from, to, value, gasLimit, maxFeePerGas, txHash }
app.post("/tx", (req, res) => {
  withAddon(res, () => {
    const tx = req.body;
    if (!tx || !tx.from || !tx.to || typeof tx.value !== "number") {
      return res.status(400).json({ error: "invalid_tx_body" });
    }
    // The addon expects a TX object; we forward it as-is
    const ok = addon.submitTransaction(tx);
    res.json({ status: ok });
  });
});

// POST /mine (trigger mining for a given miner address)
app.post("/mine", (req, res) => {
  withAddon(res, () => {
    const minerAddress = req.body && req.body.minerAddress;
    if (!minerAddress) return res.status(400).json({ error: "missing_minerAddress" });
    const minedHash = addon.buildBlock ? addon.buildBlock(minerAddress) : [];
    res.json({ mined: minedHash });
  });
});

// POST /broadcast (optional: forward a rawTxHex to the network)
app.post("/broadcast", (req, res) => {
  withAddon(res, () => {
    const rawTxHex = req.body && req.body.rawTxHex;
    if (!rawTxHex) return res.status(400).json({ error: "missing_rawTxHex" });
    const ok = addon.broadcast ? addon.broadcast(rawTxHex) : true;
    res.json({ broadcast: ok });
  });
});

// Start server
server.listen(PORT, () => {
  console.log(`Medor-Blockchain API listening on port ${PORT}`);
});

