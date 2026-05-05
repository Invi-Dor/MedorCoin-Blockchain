"use strict";

// ============================================================
// 1. DEPENDENCIES & FAIL-FAST ENV CHECK
// ============================================================
const express      = require("express");
const http         = require("http");
const axios        = require("axios");
const helmet       = require("helmet");
const cors         = require("cors");
const jwt          = require("jsonwebtoken");
const crypto       = require("crypto");
const cookieParser = require("cookie-parser");
const Redis        = require("ioredis");
const bcrypt       = require("bcrypt");
const sqlite3      = require("sqlite3").verbose();
const { Wallet }   = require("ethers");
const { RateLimiterRedis } = require("rate-limiter-flexible");
const { doubleCsrf }       = require("csrf-csrf");
const winston      = require("winston");
const Joi          = require("joi");

const REQUIRED_ENV = ["JWT_SECRET", "MNEMONIC_ENC_KEY", "CSRF_SECRET", "COOKIE_SECRET", "HCAPTCHA_SECRET", "REDIS_URL"];
for (const key of REQUIRED_ENV) {
    if (!process.env[key]) {
        console.error(`[FATAL] Missing environment variable: ${key}`);
        process.exit(1);
    }
}

// Global Config
const PORT       = process.env.PORT || 5000;
const JWT_SECRET = process.env.JWT_SECRET;
const ENC_KEY    = crypto.createHash('sha256').update(process.env.MNEMONIC_ENC_KEY).digest();

// ============================================================
// 2. AUDIT LOGGING & INFRASTRUCTURE (SQLite/Redis)
// ============================================================
const logger = winston.createLogger({
    level: "info",
    format: winston.format.combine(winston.format.timestamp(), winston.format.json()),
    transports: [new winston.transports.File({ filename: "node_audit.log" }), new winston.transports.Console()]
});

const redis = new Redis(process.env.REDIS_URL);
const db = new sqlite3.Database("./medorcoin.db");

// Hardening SQLite for concurrency
db.serialize(() => {
    db.run("PRAGMA journal_mode = WAL;");
    db.run("PRAGMA busy_timeout = 10000;");
    db.run(`CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        username TEXT UNIQUE,
        email TEXT UNIQUE,
        passwordHash TEXT,
        address TEXT UNIQUE
    )`);
});

// ============================================================
// 3. CRYPTO & BIP39 HELPERS (AES-256-GCM)
// ============================================================
const encrypt = (plaintext) => {
    const iv     = crypto.randomBytes(12);
    const cipher = crypto.createCipheriv("aes-256-gcm", ENC_KEY, iv);
    const ct     = Buffer.concat([cipher.update(plaintext, "utf8"), cipher.final()]);
    const tag    = cipher.getAuthTag();
    return [iv.toString("hex"), tag.toString("hex"), ct.toString("hex")].join(":");
};

const decrypt = (stored) => {
    const [ivHex, tagHex, ctHex] = stored.split(":");
    if (!ivHex || !tagHex || !ctHex) throw new Error("Malformed ciphertext");
    const decipher = crypto.createDecipheriv("aes-256-gcm", ENC_KEY, Buffer.from(ivHex, "hex"));
    decipher.setAuthTag(Buffer.from(tagHex, "hex"));
    return Buffer.concat([decipher.update(Buffer.from(ctHex, "hex")), decipher.final()]).toString("utf8");
};

// Redis Atomic Operation
redis.defineCommand("getAndDel", { numberOfKeys: 1, lua: "local v = redis.call('get', KEYS); if v then redis.call('del', KEYS) end; return v" });

// ============================================================
// 4. CORE SERVICES STUBS (Wired for Production)
// ============================================================
const TransactionEngine = require('./transaction_engine.cjs');
const MiningService     = require('./mining_service.cjs');
const engine = new TransactionEngine(null, JWT_SECRET, "medor-node-01");
const miner  = new MiningService(engine);

// ============================================================
// 5. SECURITY MIDDLEWARE (CSRF/Rate Limit)
// ============================================================
const app = express();
app.use(helmet());
app.use(cookieParser(process.env.COOKIE_SECRET));
app.use(express.json({ limit: "5kb" }));
app.use(cors({ origin: "https://medorcoin.org", credentials: true }));

const signupLimiter = new RateLimiterRedis({
    storeClient: redis, points: 5, duration: 3600, keyPrefix: "signup_limit"
});

const { doubleCsrfProtection, generateToken } = doubleCsrf({
    getSecret: () => process.env.CSRF_SECRET,
    cookieName: "x-csrf-token",
    cookieOptions: { httpOnly: true, secure: true, sameSite: "strict" }
});

const authGuard = (req, res, next) => {
    const token = req.cookies.medor_sid;
    if (!token) return res.status(401).json({ error: "Unauthorized" });
    try {
        req.user = jwt.verify(token, JWT_SECRET);
        next();
    } catch { res.status(401).json({ error: "Invalid Session" }); }
};

// ============================================================
// 6. PRODUCTION ROUTES
// ============================================================

// Handshake for Frontend
app.get("/api/csrf-token", (req, res) => {
    res.json({ csrfToken: generateToken(req, res) });
});

// SIGNUP: The Fully Hardened Route
app.post("/api/signup", doubleCsrfProtection, async (req, res) => {
    const { username, password, email, captchaToken } = req.body;

    try {
        // A. Rate Limit Check
        try { await signupLimiter.consume(req.ip); }
        catch (rl) { return res.status(429).json({ error: "Too many attempts. Try later." }); }

        // B. hCaptcha Verification (Correct Endpoint)
        const cRes = await axios.post("https://hcaptcha.com", 
            new URLSearchParams({ secret: process.env.HCAPTCHA_SECRET, response: captchaToken }).toString(),
            { timeout: 5000, headers: { "Content-Type": "application/x-www-form-urlencoded" } }
        );
        if (!cRes.data.success) return res.status(403).json({ error: "Captcha failed" });

        // C. Wallet Generation & Password Hashing
        const passHash = await bcrypt.hash(password, 12);
        const wallet   = Wallet.createRandom();

        // D. Transactional DB Write
        await new Promise((re, rj) => {
            db.run(`INSERT INTO users (username, email, passwordHash, address) VALUES (?,?,?,?)`, 
                [username, email, passHash, wallet.address], (err) => err ? rj(err) : re());
        });

        // E. Secure Mnemonic Claim Pattern
        const claimId = crypto.randomBytes(16).toString("hex");
        await redis.set(`claim:${claimId}`, encrypt(wallet.mnemonic.phrase), "EX", 120);

        // F. Session Issue
        const token = jwt.sign({ sub: wallet.address, iss: "medorcoin.org", aud: "medor-frontend" }, JWT_SECRET, { expiresIn: "1h" });
        res.cookie("medor_sid", token, { httpOnly: true, secure: true, sameSite: "Strict" });

        res.status(201).json({ success: true, address: wallet.address, claimId });

    } catch (err) {
        logger.error("SIGNUP_ERROR", { msg: err.message, ip: req.ip });
        res.status(400).json({ error: err.message.includes("UNIQUE") ? "Account already exists" : "Registration Error" });
    }
});

// CLAIM: Atomic reveal and burn
app.post("/api/claim-mnemonic", authGuard, doubleCsrfProtection, async (req, res) => {
    const { claimId } = req.body;
    const stored = await redis.getAndDel(`claim:${claimId}`);
    if (!stored) return res.status(404).json({ error: "Mnemonic already claimed or link expired" });

    try {
        res.json({ mnemonic: decrypt(stored) });
    } catch (e) {
        res.status(500).json({ error: "Decryption failure" });
    }
});

// MINING: Protected Route
app.get("/api/mining/status", authGuard, (req, res) => {
    res.json({ active: miner.isMining, hashRate: miner.getHashRate() });
});

// ============================================================
// 7. START SERVER
// ============================================================
const server = http.createServer(app);
server.listen(PORT, "0.0.0.0", () => {
    logger.info(`🚀 PRODUCTION NODE LIVE [PORT ${PORT}]`);
});
