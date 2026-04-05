/**
 * MEDORCOIN PRODUCTION API SERVER (v5.9.1 Sovereign - Hardened)
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
const cors = require('cors');

// Core Modules
const TransactionEngine = require('./transaction_engine.cjs');
const AuthService = require('./auth_service.cjs');
const MedorWS = require('./ws_server.cjs');

// --- Initialization ---
const NODE_ID = process.env.NODE_ID || `api-${crypto.randomBytes(3).toString('hex')}`;
const SHARED_SECRET = process.env.JWT_SECRET || "fallback_emergency_key";
const engine = new TransactionEngine(null, SHARED_SECRET, NODE_ID); 
const authService = new AuthService(engine);

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
app.use(cors());  

const wsHub = new MedorWS(server, engine);
app.use(express.json());

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

/**
 * ASYNCHRONOUS TRANSACTION GATEWAY
 * Integrates native C++ Mempool with WAL Persistence and Payload Validation
 */
app.post("/tx", async (req, res) => {
    if (!addon) return res.status(503).json({ error: "Core Addon Unavailable" });

    try {
        // 1. Request Body Validation: Fail fast before hitting the WAL
        const { from, amount, signature } = req.body;
        if (!from || amount === undefined || !signature) {
            return res.status(400).json({ 
                error: "Invalid Transaction: 'from', 'amount', and 'signature' are required." 
            });
        }

        // 2. Persistence: Write to Engine WAL before processing
        const walEntry = JSON.stringify({ data: req.body, ts: Date.now() });
        await engine.redis.lpush("mdc:wal:active", walEntry);

        // 3. Promisified addon call for C++ Mempool submission
        const success = await new Promise((resolve, reject) => {
            addon.submitTransaction(JSON.stringify(req.body), (err, result) => {
                if (err) reject(err);
                else resolve(result);
            });
        });

        if (success) {
            // 4. WAL Cleanup on Success: Prevent redundant re-processing on reboot
            await engine.redis.lrem("mdc:wal:active", 1, walEntry);

            // High-Priority Dispatch for Cluster-wide UI Update
            await globalDispatch(from, { type: 'TX_PENDING' }, 'HIGH');
            res.json({ success });
        } else {
            // Clean up WAL if transaction is rejected by the mempool logic
            await engine.redis.lrem("mdc:wal:active", 1, walEntry);
            res.status(400).json({ success: false, error: "Mempool Rejected" });
        }
    } catch (e) {
        // Note: In an extreme failure (e.g. timeout), the WAL entry remains for recoverFromCrash()
        res.status(500).json({ error: e.message });
    }
});

// --- MINING & CONSENSUS GATEWAY ---

/**
 * Provides the current block height and hash to web miners.
 */
app.get('/api/get-mining-job', async (req, res) => {
    try {
        const consensus = await getConsensusState();
        res.json({
            height: consensus.height + 1,
            blockHash: consensus.lastHash,
            difficulty: "0000" // Matches the C++ target
        });
    } catch (e) {
        res.status(500).json({ error: "Could not fetch mining job" });
    }
});

/**
 * Receives and validates Proof-of-Work from the miners.html frontend.
 * Uses the C++ addon for high-speed hash verification.
 */
app.post('/api/submit-block', async (req, res) => {
    if (!addon) return res.status(503).json({ error: "Core Addon Offline" });

    try {
        const { address, nonce, hash, height } = req.body;
        const consensus = await getConsensusState();

        // Use the C++ verifyPoW function we added to medorcoin_addon.cpp
        // Parameters: (lastBlockHash, nonce, minerAddress)
        const isValid = addon.verifyPoW(consensus.lastHash, nonce.toString(), address);

        if (isValid) {
            // Reward: Add 50 MedorCoins (using integer representation for Redis)
            await engine.redis.hincrby("mdc:balances", address, 50000000); 
            
            // Update the global stats in your 3-node Redis cluster
            await engine.redis.hset('mdc:meta:stats', 'lastMiner', address);
            
            console.log(`[BLOCK MINED] Height ${height} verified for ${address}`);
            
            // Notify other nodes/UI via Redis Pub/Sub
            await globalDispatch(address, { type: 'BLOCK_REWARD', amount: 50 }, 'HIGH');
            
            res.json({ success: true });
        } else {
            res.status(400).json({ success: false, error: "Invalid Proof of Work" });
        }
    } catch (e) {
        res.status(500).json({ error: "Internal Verification Error" });
    }
});

// ... everything above this stays the same ...

// --- LIFECYCLE MANAGEMENT ---

async function bootstrap() {
    try {
        // 1. Sync the blockchain state from Redis
        await engine.recoverFromCrash();
        
        // 2. Start the API Server
        server.listen(PORT, () => {
            console.log(`\n=========================================`);
            console.log(`🚀 MEDORCOIN API ONLINE: http://localhost:${PORT}`);
            console.log(`📡 NODE ID: ${NODE_ID}`);
            console.log(`=========================================\n`);
        });

    } catch (e) {
        console.error("❌ FATAL BOOTSTRAP ERROR:", e.message);
        process.exit(1);
    }
}

// EXECUTE THE BOOTSTRAP (ONLY ONCE)
bootstrap();

// GRACEFUL SHUTDOWN
process.on('SIGTERM', async () => {
    await engine.redis.del(`medor:node_live:${NODE_ID}`);
    server.close(() => {
        process.exit(0);
    });
});
