require('dotenv').config();
const express = require('express');
const Redis = require('ioredis');
const cors = require('cors');
const helmet = require('helmet');
const Joi = require('joi'); // Fact #2: Schema Validation
const rateLimit = require('express-rate-limit');

const app = express();

// --- 1. SCHEMAS (Fixes Risk #2: Input Validation) ---
const signupSchema = Joi.object({
    username: Joi.string().alphanum().min(3).max(30).required(),
    password: Joi.string().min(8).pattern(new RegExp('^[a-zA-Z0-9]{3,30}$')).required()
});

// --- 2. RESILIENT REDIS (Fixes Risk #3: Connection Recovery) ---
const redisConfig = {
    host: process.env.REDIS_HOST || '127.0.0.1',
    port: process.env.REDIS_PORT || 6379,
    retryStrategy(times) {
        const delay = Math.min(times * 100, 3000);
        console.warn(`🔄 Redis Reconnecting (Attempt ${times})...`);
        return delay;
    },
    maxRetriesPerRequest: null // Keep trying forever to maintain node state
};

const redis = new Redis(redisConfig);
redis.on('connect', () => console.log('✅ Node Connected to Ledger Store (Redis)'));
redis.on('error', (err) => console.error('❌ Critical Ledger Error:', err.message));

const AuthService = require('./auth_service.cjs');
const auth = new AuthService({ redis });

// --- 3. MIDDLEWARE ---
app.use(helmet());
app.use(cors());
app.use(express.json({ limit: '5kb' }));

// --- 4. HARDENED ROUTES ---

app.post('/api/auth/signup', async (req, res) => {
    // Risk #2 Fix: Validate before touching the database
    const { error, value } = signupSchema.validate(req.body);
    if (error) return res.status(400).json({ error: error.details[0].message });

    try {
        const result = await auth.signup(value.username, value.password);
        res.status(201).json(result);
    } catch (err) {
        res.status(400).json({ error: err.message });
    }
});

app.post('/api/auth/login', async (req, res) => {
    try {
        // Risk #1 Fix: Use secure login method
        const result = await auth.login(req.body.username, req.body.password, req.ip);
        res.status(200).json(result);
    } catch (err) {
        res.status(401).json({ error: "Invalid credentials" }); // Don't leak details
    }
});

app.get('/health', (req, res) => {
    const redisStatus = redis.status === 'ready' ? 'SYNCED' : 'DISCONNECTED';
    res.status(redis.status === 'ready' ? 200 : 503).json({
        node: "ONLINE",
        ledger_sync: redisStatus
    });
});

app.listen(5000, '0.0.0.0', () => console.log('🚀 FINANCIAL NODE LIVE ON 5000'));
