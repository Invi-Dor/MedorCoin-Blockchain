/**
 * MEDORCOIN AUTH SERVICE - CJS
 * Handles Wallet Generation (Ethers.js), Session Persistence, and Identity Mapping.
 */

const { ethers } = require('ethers');
const crypto = require('crypto');

class AuthService {
    constructor(engine) {
        // PROBLEM FIX: Ensures we share the Master/Server LevelDB instance
        this.engine = engine; 
    }

    // --- REAL WALLET GENERATION ---
    async signup(username, password) {
        // Generate a true BIP-39 HD Wallet
        const wallet = ethers.Wallet.createRandom();
        
        const userData = {
            username,
            address: wallet.address,
            mnemonic: wallet.mnemonic.phrase, // CRITICAL: User must save this for recovery
            privateKey: wallet.privateKey,
            createdAt: Date.now()
        };

        // 1. Store the full user profile indexed by their Blockchain Address
        await this.engine.db.put(`u:${wallet.address}`, JSON.stringify(userData));
        
        // 2. Map Username to Address to facilitate login lookups
        await this.engine.db.put(`auth:${username}`, JSON.stringify({ 
            password, 
            address: wallet.address 
        }));
        
        return userData;
    }

    // --- SESSION MANAGEMENT ---
    async login(username, password) {
        const authData = await this.engine.db.get(`auth:${username}`).catch(() => null);
        if (!authData) throw new Error("Invalid Credentials");

        const auth = JSON.parse(authData);
        if (auth.password !== password) throw new Error("Invalid Credentials");
        
        // Generate a cryptographically secure session token
        const token = crypto.randomBytes(32).toString('hex');
        
        // Store session for the API Server and Mining Master to verify
        await this.engine.db.put(`session:${token}`, auth.address);
        
        return { token, address: auth.address };
    }
}

module.exports = AuthService;
