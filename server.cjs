/**
 * MEDORCOIN PRODUCTION API SERVER (v5.9.1 Sovereign - Hardened)
 * Role: State Observer, Auth Gateway, & Global Dispatcher
 */

"use strict";

require("dotenv").config();

// We use a try-block around the requires to catch missing dependencies in the logs
try {
    const express = require("express");
    const http = require("http");
    const path = require("path");
    const crypto = require("crypto");
    const msgpack = require("msgpack5")();
    const jwt = require("jsonwebtoken");
    const cors = require('cors');

    // Core Modules - Ensure these filenames match your folder exactly
    const TransactionEngine = require('./transaction_engine.cjs');
    const AuthService = require('./auth_service.cjs');
    const MedorWS = require('./ws_server.cjs');

    // --- Initialization ---
    const NODE_ID = process.env.NODE_ID || `api-${crypto.randomBytes(3).toString('hex')}`;
    const SHARED_SECRET = process.env.JWT_SECRET || "fallback_emergency_key";
    const PORT = process.env.PORT || 5000;

    const engine = new TransactionEngine(null, SHARED_SECRET, NODE_ID); 
    const authService = new AuthService(engine);

    // --- C++ Addon Handling ---
    let addon;
    try {
        addon = require(path.join(__dirname, "build", "Release", "medorcoin_addon.node"));
        console.log("✅ Medor Core Addon loaded successfully.");
    } catch (e) {
        console.warn("[WARN] Medor Core Addon not found, falling back to JS-only mode.");
        addon = null; 
    }

    // --- Express & Server Setup ---
    const app = express();
    const server = http.createServer(app);

    // Middleware
    app.use(cors());
    app.use(express.json());
    app.use(express.static(__dirname));

    // WebSocket Initialization
    const wsHub = new MedorWS(server, engine);

    // --- GLOBAL DISPATCH ENGINE ---
    async function globalDispatch(address, data, priority = 'LOW') {
        const targetNode = await engine.redis.get(`medor:registry:users:${address}`).catch(() => null);
        if (!targetNode) return; 
        const payload = msgpack.encode({ address, priority, ...data });
        await engine.redis.publish(`medor:node_bus:${targetNode}`, payload);
    }

    // --- OBSERVATION LAYER ---
    async function getConsensusState() {
        try {
            const stats = await engine.redis.hgetall('mdc:meta:stats').catch(() => ({}));
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

    // --- ROUTES ---

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

    app.post('/api/signup', async (req, res) => {
        try {
            const { username, password } = req.body;
            const user = await authService.signup(username, password);
            res.json({ success: true, address: user.address, mnemonic: user.mnemonic });
        } catch (e) { res.status(400).json({ success: false, error: e.message }); }
    });

    app.post('/api/login', async (req, res) => {
        try {
            const { username, password } = req.body;
            const session = await authService.login(username, password);
            res.json({ success: true, token: session.token, address: session.address });
        } catch (e) { res.status(401).json({ success: false, error: "Authentication failed" }); }
    });

    app.get('/api/user-stats', async (req, res) => {
        try {
            const { addr, token } = req.query;
            if (!addr || !token) return res.status(400).json({ error: "Missing parameters" });
            const decoded = jwt.verify(token, SHARED_SECRET);
            if (decoded.wallet !== addr) return res.status(401).json({ error: "Unauthorized" });

            const balanceRaw = await engine.redis.hget("mdc:balances", addr).catch(() => "0");
            const state = await engine.redis.hgetall(`user:${addr}:state`).catch(() => ({}));
            const consensus = await getConsensusState();

            res.json({
                success: true,
                balance: balanceRaw, 
                units: "atomic",
                speed: state.allocatedSpeed || 0,
                network: consensus
            });
        } catch (e) { res.status(500).json({ error: "Internal State Error" }); }
    });

    app.post('/api/submit-block', async (req, res) => {
        try {
            const { address, nonce } = req.body;
            if (!addon) {
                // Manual fallback logic if addon is missing
                await engine.redis.hincrby("mdc:balances", address, 50000000);
                return res.json({ success: true, note: "JS-Fallback Reward" });
            }
            const consensus = await getConsensusState();
            const isValid = addon.verifyPoW(consensus.lastHash, nonce.toString(), address);

            if (isValid) {
                await engine.redis.hincrby("mdc:balances", address, 50000000); 
                await engine.redis.hset('mdc:meta:stats', 'lastMiner', address);
                await globalDispatch(address, { type: 'BLOCK_REWARD', amount: 50 }, 'HIGH');
                res.json({ success: true });
            } else {
                res.status(400).json({ success: false, error: "Invalid Proof of Work" });
            }
        } catch (e) { res.status(500).json({ error: e.message }); }
    });

    // --- LIFECYCLE & STARTUP ---
    async function bootstrap() {
        console.log(`⏳ Node ${NODE_ID} Bootstrapping...`);
        
        // Attempt engine recovery with a timeout
        await Promise.race([
            engine.recoverFromCrash(),
            new Promise((resolve) => setTimeout(resolve, 3000)) 
        ]).catch(err => console.error("Recovery error:", err));

        server.listen(PORT, '0.0.0.0', () => {
            console.log(`🚀 MEDORCOIN GATEWAY LIVE ON PORT ${PORT}`);
        });
    }

    bootstrap();

    // Graceful Shutdown
    process.on('SIGTERM', async () => {
        try { await engine.redis.del(`medor:node_live:${NODE_ID}`); } catch (e) {}
        server.close(() => process.exit(0));
    });

} catch (fatalError) {
    console.error("--- CRITICAL LOAD ERROR ---");
    console.error(fatalError.message);
    console.error(fatalError.stack);
    process.exit(1);
}
