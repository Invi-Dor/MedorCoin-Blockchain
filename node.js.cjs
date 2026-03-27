/**
 * MedorCoin - Industrial Shield Node (Final V16)
 * Filename: /workspaces/MedorCoin-Blockchian/node.js.cjs
 */

const { Worker, isMainThread, parentPort } = require("worker_threads");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const express = require('express');
const ethers = require('ethers');

// ─── 1. GLOBAL CHAOS HANDLER (Anti-Crash) ─────────────────────────
process.on("uncaughtException", (err) => console.error("[FATAL_CRASH_PREVENTED]", err.stack || err));
process.on("unhandledRejection", (r) => console.error("[PROMISE_REJECTION_PREVENTED]", r));

// ─── 2. CONFIGURATION ──────────────────────────────────────────────
const DB_PATH = path.join(__dirname, "user_registry.json");
const CONFIG = {
    apiPort: 3000,
    workerCount: parseInt(process.env.WORKERS || "4"),
    decimals: 100000000n,      // 10^8
    speedScale: 1000n,         // 1.000 GH/s
    miningRate: 10n,           // 10 Satoshi / 1 GH/s / Sec
    sessionTTL: 86400000,      // 24h
    saveInterval: 300000       // 5 Minutes
};

const TIERS = {
    FREE: { speed: 4000n, cooldown: 43200000, duration: 43200000 },
    PRO:  { speed: 600000n, cooldown: 86400000, duration: 86400000 }
};

let USER_DATA = new Map(); 
const SESSION_TOKENS = new Map();

// ─── 3. PERSISTENCE ───────────────────────────────────────────────
function saveToDisk() {
    try {
        const data = {};
        for (let [k, v] of USER_DATA) data[k] = { ...v, balance: v.balance.toString() };
        fs.writeFileSync(DB_PATH, JSON.stringify(data, null, 2));
        console.log(`[DB] Checkpoint saved.`);
    } catch (e) { console.error("[DB_ERR]", e.message); }
}

function loadFromDisk() {
    try {
        if (!fs.existsSync(DB_PATH)) return;
        const raw = JSON.parse(fs.readFileSync(DB_PATH, 'utf8'));
        for (let k in raw) {
            raw[k].balance = BigInt(raw[k].balance || "0");
            USER_DATA.set(k, raw[k]);
        }
        console.log(`[DB] Restored ${USER_DATA.size} users.`);
    } catch (e) { console.error("[DB_LOAD_ERR]", e.message); }
}

// ─── 4. UTILITIES ────────────────────────────────────────────────
function formatSatoshi(amount) {
    const s = amount.toString().padStart(9, '0');
    return s.slice(0, -8) + "." + s.slice(-8);
}

// ─── 5. WORKER THREAD ─────────────────────────────────────────────
if (!isMainThread) {
    parentPort.on("message", ({ type, payload, id }) => {
        try {
            if (type === "verify_pow") {
                const res = _workerVerifyPoW(payload.header, payload.nBits);
                parentPort.postMessage({ id, result: res });
            }
        } catch (e) { parentPort.postMessage({ id, error: e.message }); }
    });
    function _workerVerifyPoW(h, nBits) {
        const header = Buffer.alloc(80);
        header.writeUInt32LE(h.version || 1, 0);
        if (h.prevHash) Buffer.from(h.prevHash, "hex").copy(header, 4);
        if (h.merkleRoot) Buffer.from(h.merkleRoot, "hex").copy(header, 36);
        header.writeUInt32LE(h.timestamp || 0, 68);
        header.writeUInt32LE(nBits || 0, 72);
        header.writeBigUInt64LE(BigInt(h.nonce || 0), 76);
        const h1 = crypto.createHash("sha256").update(header).digest();
        const hash = crypto.createHash("sha256").update(h1).digest();
        const hashBI = BigInt("0x" + Buffer.from(hash).reverse().toString("hex"));
        const target = BigInt(nBits & 0xffffff) * (2n ** (8n * BigInt(((nBits >> 24) & 0xff) - 3)));
        return hashBI <= target;
    }
    return;
}

// ─── 6. WORKER POOL ───────────────────────────────────────────────
class WorkerPool {
    constructor(size) {
        this.workers = []; this.queue = []; this._pending = new Map(); this._seq = 0;
        for (let i = 0; i < size; i++) {
            const w = new Worker(__filename);
            w.on("message", (m) => this._handle(w, m));
            w._busy = false; this.workers.push(w);
        }
    }
    _handle(w, { id, result, error }) {
        const h = this._pending.get(id);
        if (h) { this._pending.delete(id); error ? h.reject(error) : h.resolve(result); }
        if (this.queue.length > 0) {
            const { msg, resolve, reject } = this.queue.shift();
            this._dispatch(w, msg, resolve, reject);
        } else w._busy = false;
    }
    run(type, payload) {
        return new Promise((resolve, reject) => {
            const msg = { type, payload, id: ++this._seq };
            const free = this.workers.find(w => !w._busy);
            if (free) this._dispatch(free, msg, resolve, reject);
            else this.queue.push({ msg, resolve, reject });
        });
    }
    _dispatch(w, msg, resolve, reject) {
        w._busy = true; this._pending.set(msg.id, { resolve, reject });
        w.postMessage(msg);
    }
    async terminate() { await Promise.all(this.workers.map(w => w.terminate())); }
}
const pool = new WorkerPool(CONFIG.workerCount);

// ─── 7. INDUSTRIAL API ────────────────────────────────────────────
const getSafeUser = (addr) => {
    const key = String(addr || "").toLowerCase();
    if (!USER_DATA.has(key)) {
        USER_DATA.set(key, { balance: 0n, lastSync: Date.now(), proExpiry: 0, claims: { FREE: 0, PRO: 0 }, contracts: [] });
    }
    return USER_DATA.get(key);
};

const validateToken = (addr, token) => {
    if (typeof addr !== 'string' || typeof token !== 'string') return false;
    const s = SESSION_TOKENS.get(token);
    if (!s || s.addr !== addr.toLowerCase() || s.expires < Date.now()) return false;
    s.expires = Date.now() + CONFIG.sessionTTL;
    return true;
};

setInterval(() => {
    const now = Date.now();
    for (const [t, s] of SESSION_TOKENS.entries()) if (s.expires < now) SESSION_TOKENS.delete(t);
}, 3600000);

setInterval(saveToDisk, CONFIG.saveInterval);

const app = express();
app.use(express.json());

app.post('/api/auth', async (req, res) => {
    try {
        const { addr, signature, message } = req.body;
        if (typeof addr !== 'string' || !signature || !message) throw new Error();
        const recovered = ethers.verifyMessage(message, signature);
        if (recovered.toLowerCase() !== addr.toLowerCase()) throw new Error();
        const token = crypto.randomBytes(32).toString('hex');
        SESSION_TOKENS.set(token, { addr: addr.toLowerCase(), expires: Date.now() + CONFIG.sessionTTL });
        getSafeUser(addr);
        res.json({ token });
    } catch (e) { res.status(401).json({ error: "UNAUTHORIZED" }); }
});

app.get('/api/user-stats', (req, res) => {
    try {
        const { addr, token } = req.query;
        if (!validateToken(addr, token)) return res.status(401).send();
        const user = getSafeUser(addr);
        const now = Date.now();
        user.contracts = (user.contracts || []).filter(c => c && c.expiry > now);
        
        const totalSpeedScaled = 500n + user.contracts.reduce((s, c) => s + BigInt(c.speed || 0), 0n);
        
        // SHIELD FIX 1: Defensive guard against negative or fractional elapsed time
        const diff = now - user.lastSync;
        const elapsed = BigInt(Math.max(0, Math.floor(diff / 1000)));
        
        if (elapsed > 0n) {
            user.balance += (elapsed * totalSpeedScaled * CONFIG.miningRate) / CONFIG.speedScale;
            user.lastSync = now;
        }
        res.json({
            balance: formatSatoshi(user.balance),
            speed: (Number(totalSpeedScaled) / 1000).toFixed(3),
            proActive: (user.proExpiry || 0) > now,
            proExpiryDays: Math.ceil(Math.max(0, (user.proExpiry || 0) - now) / 86400000),
            freeCooldown: Math.max(0, TIERS.FREE.cooldown - (now - (user.claims.FREE || 0))),
            proCooldown: Math.max(0, TIERS.PRO.cooldown - (now - (user.claims.PRO || 0)))
        });
    } catch (e) { res.status(500).json({ error: "NODE_ERR" }); }
});

app.post('/api/action', (req, res) => {
    try {
        const { addr, token, type } = req.body;
        if (!validateToken(addr, token) || typeof type !== 'string') throw new Error();
        const user = getSafeUser(addr);
        const now = Date.now();
        const tier = (type === 'claim-free') ? 'FREE' : 'PRO';
        const cfg = TIERS[tier];

        // SHIELD FIX 2: Check for valid proExpiry
        if (tier === 'PRO' && (!user.proExpiry || user.proExpiry < now)) return res.json({ error: "PRO_REQUIRED" });
        if (now - (user.claims[tier] || 0) < cfg.cooldown) return res.json({ error: "COOLDOWN" });

        user.claims[tier] = now;
        
        // SHIELD FIX 3: Correctly update proExpiry to reflect claimed time
        if (tier === 'PRO') user.proExpiry = now + cfg.duration;
        
        user.contracts.push({ type: tier, speed: cfg.speed, expiry: now + cfg.duration });
        res.json({ success: true });
    } catch (e) { res.status(500).json({ error: "ACTION_FAIL" }); }
});

// ─── 8. STARTUP & SHUTDOWN ────────────────────────────────────────
const shutdown = async () => {
    saveToDisk();
    await pool.terminate();
    process.exit(0);
};
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

loadFromDisk();
app.listen(CONFIG.apiPort, () => console.log(`[NODE] Medor V16 | Port ${CONFIG.apiPort}`));
