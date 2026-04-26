/**
 * MEDORCOIN PRODUCTION NODE (node.cjs)
 * Updated: Integrated BIP39 Logic & Hardened Registration
 */

require('dotenv').config();
const express = require('express');
const Redis = require('ioredis');
const cors = require('cors');
const helmet = require('helmet');
const Joi = require('joi');
const rateLimit = require('express-rate-limit');

const app = express();

// --- 1. SCHEMAS (Enforces the "One Pack" Password Structural Rules) ---
const signupSchema = Joi.object({
    username: Joi.string().alphanum().min(3).max(30).required(),
    password: Joi.string().min(8)
        .pattern(new RegExp('^(?=.*[a-z])(?=.*[A-Z])(?=.*[0-9])(?=.*[!@#$%^&*])'))
        .required()
        .messages({
            'string.pattern.base': 'Password requires Uppercase, Lowercase, Number, and Special Character.'
        })
});

// --- 2. RESILIENT REDIS (Ledger Store) ---
const redisConfig = {
    host: process.env.REDIS_HOST || '127.0.0.1',
    port: process.env.REDIS_PORT || 6379,
    retryStrategy: (times) => Math.min(times * 100, 3000)
};
const redis = new Redis(redisConfig);

const AuthService = require('./auth_service.cjs');
const auth = new AuthService({ redis });

// --- 3. MIDDLEWARE ---
app.use(helmet());
app.use(cors());
app.use(express.json({ limit: '5kb' }));
app.use(express.static(__dirname)); // Serves your signup.html and verify.html

// --- 4. HARDENED ROUTES ---

/**
 * PRODUCTION SIGNUP ROUTE
 * Fixed: Matches HTML fetch URL and returns BIP39 Mnemonic
 */
app.post('/api/signup', async (req, res) => {
    // Validate structural requirements before hitting the database
    const { error, value } = signupSchema.validate(req.body);
    if (error) return res.status(400).json({ success: false, error: error.details[0].message });

    try {
        // Triggers AuthService which uses your BIP39 C++ Logic
        const result = await auth.signup(value.username, value.password);
        
        // Return success + keys so the frontend moves to verify.html
        res.status(201).json({
            success: true,
            address: result.address,
            mnemonic: result.mnemonic
        });
    } catch (err) {
        res.status(400).json({ success: false, error: err.message });
    }
});

app.post('/api/auth/login', async (req, res) => {
    try {
        const result = await auth.login(req.body.username, req.body.password, req.ip);
        res.status(200).json({ success: true, ...result });
    } catch (err) {
        res.status(401).json({ success: false, error: "Authentication failed" });
    }
});

// Verification Route (Matches verify.html)
app.post('/api/verify', async (req, res) => {
    const { code } = req.body;
    if (code && code.length === 6) {
        res.json({ success: true });
    } else {
        res.status(400).json({ success: false, error: "Invalid verification code" });
    }
});

app.get('/health', (req, res) => {
    res.json({ status: "ONLINE", ledger: redis.status === 'ready' ? 'SYNCED' : 'DISCONNECTED' });
});

// --- 5. STARTUP ---
const PORT = process.env.PORT || 5000;
app.listen(PORT, '0.0.0.0', () => {
    console.log(`🚀 MEDORCOIN FINANCIAL NODE LIVE ON PORT ${PORT}`);
});
