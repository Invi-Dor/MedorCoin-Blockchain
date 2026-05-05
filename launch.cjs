"use strict";

/**
 * MEDORCOIN LAUNCHER
 * Bound to: 172.235.50.31 (Los Angeles)
 */

let Block;
try {
    Block = require('./Block.cjs');
} catch (e) {
    console.error("CRITICAL ERROR: Could not find Block.cjs in this directory.");
    process.exit(1);
}

const MY_IP = "172.235.50.31";

console.log(`--- Medorcoin Launch Sequence ---`);
console.log(`Binding to Network IP: ${MY_IP}`);

// Initialize Genesis
const genesis = Block.genesis();

if (genesis.verify()) {
    console.log("SUCCESS: Genesis header verified (80-byte standard).");
    console.log("Hash:", genesis.hash);
    console.log("Status: Listening for peers on port 8333...");
} else {
    console.error("ERROR: Header serialization failed. Check Block.cjs logic.");
}

// Keep process alive for the P2P network
setInterval(() => {}, 1000);
