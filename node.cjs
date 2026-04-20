require('dotenv').config();
const express = require('express');
const path = require('path');
const Redis = require('ioredis');

// Initialize Express
const app = express();
app.use(express.json());

// 1. Setup the Engine/Redis that your AuthService expects
const engine = {
    redis: new Redis(process.env.REDIS_URL || 'redis://localhost:6379') // Connects to Railway Redis
};

// 2. Initialize your awesome Auth Service
const AuthService = require('./auth_service.cjs');
const auth = new AuthService(engine);

// 3. Create the Express Routes that use your class
app.post('/api/auth/signup', async (req, res) => {
    try {
        const result = await auth.signup(req.body.username, req.body.password);
        res.status(201).json(result);
    } catch (error) {
        res.status(400).json({ error: error.message });
    }
});

app.post('/api/auth/login', async (req, res) => {
    try {
        const result = await auth.login(req.body.username, req.body.password, req.ip);
        res.status(200).json(result);
    } catch (error) {
        res.status(401).json({ error: error.message });
    }
});

// 4. Load the C++ MedorCoin Addon (if available)
try {
    const medorAddon = require('./build/Release/medorcoin_addon.node');
    console.log("✅ MedorCoin C++ Addon Loaded Successfully");
} catch (err) {
    console.error("⚠️ C++ Addon not loaded yet (expected if compiling):", err.message);
}

// 5. Health Check
app.get('/status', (req, res) => {
    res.json({
        status: "LIVE",
        blockchain: "MedorCoin",
        version: "V6-Industrial",
        network: "Mainnet"
    });
});

const PORT = 5000;
app.listen(PORT, '0.0.0.0', () => {
    console.log(`LIVE ON PORT ${PORT}`);
});

