/**
 * MedorCoin - Industrial Server (Final V20)
 * Filename: node.js.cjs
 */

try {
    // We wrap the configuration and any potential requires here
    // to catch "Module Not Found" or "BigInt Serialization" errors.

    const CONFIG = {
        apiPort: 5000,             // Updated to 5000 to match your EXPOSE and Railway config
        decimals: 100000000n,      // 10^8 (1.00000000 MEDOR)
        miningRate: 10n,           // 10 Satoshi per 1 GH/s per second
        speedScale: 1000n,         // Internal multiplier for precision
        sessionTTL: 86400000       // 24h
    };

    const TIERS = {
        FREE:      { speed: 4000n,   cooldown: 43200000, duration: 43200000 }, 
        MONTHLY:   { speed: 600000n, cooldown: 0,        duration: 2592000000 }, 
        QUARTERLY: { speed: 600000n, cooldown: 0,        duration: 7776000000 }, 
        YEARLY:    { speed: 600000n, cooldown: 0,        duration: 31536000000 } 
    };

    console.log("Configuration loaded successfully on port:", CONFIG.apiPort);

    // If you have requires, put them here, for example:
    // const db = require('./db.cjs');

} catch (err) {
    console.error("--- FATAL STARTUP ERROR ---");
    console.error("MESSAGE:", err.message);
    console.error("STACK:", err.stack);
    process.exit(1);
}


// THE FIGURES YOU REQUESTED
const TIERS = {
    FREE:      { speed: 4000n,   cooldown: 43200000, duration: 43200000 }, // 4 GH/s (12h)
    MONTHLY:   { speed: 600000n, cooldown: 0,        duration: 2592000000 }, // 600 GH/s (30d)
    QUARTERLY: { speed: 600000n, cooldown: 0,        duration: 7776000000 }, // 600 GH/s (90d)
    YEARLY:    { speed: 600000n, cooldown: 0,        duration: 31536000000 } // 600 GH/s (365d)
};

// Calculation Logic (Used in /api/user-stats)
// balance += (elapsed_seconds * total_ghs * 10_Satoshi)
