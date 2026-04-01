/**
 * MedorCoin - Industrial Server (Final V20)
 * Filename: /workspaces/MedorCoin-Blockchian/node.js.cjs
 */

const CONFIG = {
    apiPort: 3000,
    decimals: 100000000n,      // 10^8 (1.00000000 MEDOR)
    miningRate: 10n,           // 10 Satoshi per 1 GH/s per second
    speedScale: 1000n,         // Internal multiplier for precision
    sessionTTL: 86400000       // 24h
};

// THE FIGURES YOU REQUESTED
const TIERS = {
    FREE:      { speed: 4000n,   cooldown: 43200000, duration: 43200000 }, // 4 GH/s (12h)
    MONTHLY:   { speed: 600000n, cooldown: 0,        duration: 2592000000 }, // 600 GH/s (30d)
    QUARTERLY: { speed: 600000n, cooldown: 0,        duration: 7776000000 }, // 600 GH/s (90d)
    YEARLY:    { speed: 600000n, cooldown: 0,        duration: 31536000000 } // 600 GH/s (365d)
};

// Calculation Logic (Used in /api/user-stats)
// balance += (elapsed_seconds * total_ghs * 10_Satoshi)
