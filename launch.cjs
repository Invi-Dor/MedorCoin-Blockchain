"use strict";

const Block = require('./Block.cjs'); // The file you shared
const MY_VPN_IP = "85.155.186.123";

console.log(`--- Medorcoin Launch Sequence ---`);
console.log(`Binding to VPN IP: ${MY_VPN_IP}`);

// Initialize Genesis
const genesis = Block.genesis();

if (genesis.verify()) {
    console.log("SUCCESS: Genesis header verified (80-byte standard).");
    console.log("Hash:", genesis.hash);
    console.log("Status: Listening for peers via NymVPN...");
} else {
    console.error("ERROR: Header serialization failed.");
}
