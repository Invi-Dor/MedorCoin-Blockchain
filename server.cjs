/**
 * MEDORCOIN PRODUCTION API SERVER (v5.9 Sovereign)
 * Role: State Observer, Auth Gateway, & Global Dispatcher
 */

"use strict";

require("dotenv").config();
const express = require("express");
const http = require("http");
const path = require("path");
const crypto = require("crypto");
const msgpack = require("msgpack5")();
const jwt = require("jsonwebtoken");

// Core Modules
const TransactionEngine = require('./transaction_engine.cjs');
const AuthService = require('./auth_service.cjs');
const MedorWS = require('./ws_server.cjs'); // The Priority-Aware Hub we built

// --- Initialization ---
const NODE_ID = process.env.NODE_ID || `api-${crypto.randomBytes(3).toString('hex')}`;
const engine = new TransactionEngine({ nodeId: NODE_ID });
const authService = new AuthService(engine);

// Native C++ Core Integration
let addon;
try {
    addon = require(path.join(__dirname, "build", "Release", "medorcoin_addon.node"));
} catch (e) {
    console.warn("[WARN] Medor Core Addon not found, falling back to JS-only mode.");
    addon = null; 
}

const app = express();
const server = http.createServer(app);
const PORT = process.env.PORT || 5000;

// Initialize Distributed WebSocket Hub
const wsHub = new MedorWS(server, engine);

app.use(express.json());

// --- GLOBAL DISPATCH ENGINE ---
/**
 * Dispatches updates to the correct node in the global fabric.
 * Ensures immediate consistency across the cluster.
 */
async function globalDispatch(address, data, priority = 'LOW') {
    // 1. Locate which node the user is currently connected to
    const targetNode = await engine.db.get(`medor:registry:users:${address}`).catch(() => null);
    if (!targetNode) return; // User is offline cluster-wide

    // 2. Wrap in MsgPack Binary for High-Speed Bus Transfer
    const payload = msgpack.encode({ address, priority, ...data });
    
    // 3. Publish to the target node's specific bus
    await engine.db.publish(`medor:node_bus:${targetNode}`, payload);
}

// --- OBSERVATION LAYER ---
async function getConsensusState() {
    try {
        const stats = await engine.db.get('network:stats').catch(() => ({}));
        return {
            height: Number(engine.currentHeight) || 0,
            lastHash: String(engine.lastBlockHash || "0".repeat(64)),
            difficulty: stats.difficulty || "6",
            anomaly_detected: !!stats.fallbackAlert 
        };
    } catch (e) {
        return { height: 0, lastHash: "0".repeat(64), difficulty: "6", anomaly_detected: false };
    }
}

// --- AUTHENTICATION ROUTES ---

app.post('/api/signup', async (req, res) => {
    try {
        const { username, password } = req.body;
        const user = await authService.signup(username, password);
        res.json({ success: true, address: user.address, mnemonic: user.mnemonic });
    } catch (e) { 
        res.status(400).json({ success: false, error: e.message }); 
    }
});

app.post('/api/login', async (req, res) => {
    try {
        const { username, password } = req.body;
        const session = await authService.login(username, password);
        // session.token is a signed JWT
        res.json({ success: true, token: session.token, address: session.address });
    } catch (e) {
        res.status(401).json({ success: false, error: "Authentication failed" });
    }
});

// --- USER & NETWORK STATE ---

app.get('/api/user-stats', async (req, res) => {
    try {
        const { addr, token } = req.query;
        if (!addr || !token) return res.status(400).json({ error: "Missing parameters" });

        // Verify JWT via AuthService/Engine
        const decoded = jwt.verify(token, process.env.JWT_SECRET);
        if (decoded.wallet !== addr) return res.status(401).json({ error: "Unauthorized" });

        // Fetch state from Master partition
        const state = await engine.db.get(`${engine.PARTITIONS.STATE}${addr}`).catch(() => ({ balance: "0" }));
        const consensus = await getConsensusState();

        res.json({
            success: true,
            balance: (BigInt(state.balance || 0) / 100000000n).toString(),
            speed: state.allocatedSpeed || 0,
            network: consensus
        });
    } catch (e) {
        res.status(500).json({ error: "Internal State Error" });
    }
});

// --- CORE GATEWAY & TRANSACTION ROUTES ---

app.get("/health", async (req, res) => {
    const consensus = await getConsensusState();
    res.json({ 
        status: "ok", 
        node_id: NODE_ID,
        height: consensus.height, 
        core_ready: !!addon,
        ws_clients: wsHub.localClients.size
    });
});

app.post("/tx", async (req, res) => {
    if (!addon) return res.status(503).json({ error: "Core Addon Unavailable" });

    // 1. Submit to Addon's high-speed Mempool (C++)
    const success = addon.submitTransaction(req.body);
    
    if (success) {
        // 2. If tx changes user balance, trigger Global Dispatch (High Priority)
        // This ensures the dashboard updates instantly across nodes.
        await globalDispatch(req.body.from, { 
            type: 'TX_PENDING', 
            hash: req.body.hash 
        }, 'HIGH');
    }

    res.json({ success });
});

// --- LIFECYCLE MANAGEMENT ---

async function bootstrap() {
    try {
        // Recover state & verify DB partitions
        await engine.recoverFromCrash().catch(() => {});
        
        server.listen(PORT, () => {
            console.log(`\n[SOVEREIGN-API] Node: ${NODE_ID}`);
            console.log(`[SOVEREIGN-API] Observer active on port ${PORT}\n`);
        });
    } catch (err) {
        console.error("[FATAL] Bootstrap failed:", err);
        process.exit(1);
    }
}

// Graceful Shutdown (Parity with Master Node)
process.on('SIGTERM', async () => {
    console.log("[API] Commencing graceful shutdown...");
    // 1. Unregister node from global liveness
    await engine.db.del(`medor:node_live:${NODE_ID}`);
    // 2. Close connections
    server.close(() => {
        console.log("[API] Offline.");
        process.exit(0);
    });
});

bootstrap();
