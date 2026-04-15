"use strict";

/**
 * MEDORCOIN - Industrial Configuration (Hardened)
 * Logic: All numerical values use BigInt (n) to ensure 100% precision 
 * in global financial state calculations across all nodes.
 */

const TIERS = Object.freeze({
    FREE: Object.freeze({ 
        speed: 4000n,   
        cooldown: 43200000n, 
        duration: 43200000n 
    }),
    MONTHLY: Object.freeze({ 
        speed: 600000n, 
        cooldown: 0n,        
        duration: 2592000000n 
    }),
    QUARTERLY: Object.freeze({ 
        speed: 600000n, 
        cooldown: 0n,        
        duration: 7776000000n 
    }),
    YEARLY: Object.freeze({ 
        speed: 600000n, 
        cooldown: 0n,        
        duration: 31536000000n 
    })
});

const CONFIG = Object.freeze({
    apiPort: 3000,              // Keep as Number for network libs
    decimals: 100000000n,      // 10^8 (1.00000000 MEDOR)
    miningRate: 10n,           // 10 Satoshi per 1 GH/s per second
    speedScale: 1000n,         // Internal multiplier for precision
    sessionTTL: 86400000n      // 24h in ms
});

// Export as a unified, read-only manifest
module.exports = {
    TIERS,
    CONFIG
};
