require('dotenv').config();
const express = require('express');
const { ethers } = require('ethers');
const path = require('path');

// Initialize Express
const app = express();
app.use(express.json());

// Load the C++ MedorCoin Addon
let medorAddon;
try {
    medorAddon = require('./build/Release/medorcoin_addon.node');
    console.log("✅ MedorCoin C++ Addon Loaded Successfully");
} catch (err) {
    console.error("❌ Failed to load C++ Addon:", err.message);
}

// Intertwine the Auth Service
const authService = require('./auth_service.cjs');
app.use('/api/auth', authService);

// Health Check / Blockchain Status
app.get('/status', (req, res) => {
    res.json({
        status: "LIVE",
        blockchain: "MedorCoin",
        version: "V6-Industrial",
        network: "Mainnet"
    });
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
    console.log(`🚀 MEDORCOIN GATEWAY LIVE ON PORT ${PORT}`);
});
