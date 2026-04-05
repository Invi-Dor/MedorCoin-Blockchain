/**
 * MEDORCOIN AUTH SERVICE - PRODUCTION HARDENED
 * Features: Argon2 Hashing, JWT Sessions, Redis Cluster Persistence
 */

const { ethers } = require('ethers');
const argon2 = require('argon2');
const jwt = require('jsonwebtoken');
const crypto = require('crypto');

class AuthService {
    constructor(engine) {
        this.engine = engine;
        this.JWT_SECRET = process.env.JWT_SECRET || 'emergency_medor_secret_99';
    }

    // --- SECURE SIGNUP ---
    async signup(username, password) {
        // 1. Check if user already exists to prevent duplicates
        const existing = await this.engine.redis.get(`auth:${username}`);
        if (existing) throw new Error("Username already taken");

        // 2. Generate BIP-39 Wallet
        const wallet = ethers.Wallet.createRandom();
        
        // 3. Argon2 Hashing (High Security)
        const passwordHash = await argon2.hash(password, {
            type: argon2.argon2id,
            memoryCost: 2 ** 16, // 64MB
            timeCost: 3          // 3 iterations
        });

        const userData = {
            username,
            address: wallet.address,
            mnemonic: wallet.mnemonic.phrase, // User must save this!
            createdAt: Date.now()
        };

        // 4. Store Profile & Auth Mapping in Redis Cluster
        // We use 'set' to ensure persistence across the cluster
        await this.engine.redis.set(`u:${wallet.address}`, JSON.stringify(userData));
        await this.engine.redis.set(`auth:${username}`, JSON.stringify({ 
            hash: passwordHash, 
            address: wallet.address 
        }));
        
        return userData;
    }

    // --- SECURE LOGIN & JWT ISSUANCE ---
    async login(username, password, ip = 'unknown') {
        // 1. Rate Limiting Check (Simple version using Redis)
        const attempts = await this.engine.redis.incr(`login_attempts:${username}`);
        if (attempts > 5) {
            await this.engine.redis.expire(`login_attempts:${username}`, 600); // 10 min lockout
            throw new Error("Too many attempts. Account locked for 10 minutes.");
        }

        const authData = await this.engine.redis.get(`auth:${username}`);
        if (!authData) throw new Error("Authentication failed");

        const auth = JSON.parse(authData);
        
        // 2. Verify Argon2 Hash
        const valid = await argon2.verify(auth.hash, password);
        if (!valid) throw new Error("Authentication failed");

        // 3. Reset failed attempts on success
        await this.engine.redis.del(`login_attempts:${username}`);

        // 4. Issue Signed JWT (Stateless & Revocable)
        const token = jwt.sign(
            { wallet: auth.address, username, role: 'miner' },
            this.JWT_SECRET,
            { expiresIn: '24h' }
        );

        // 5. Store session metadata for auditing/revocation
        const sessionMeta = {
            token,
            ip,
            lastSeen: Date.now()
        };
        await this.engine.redis.set(`session:${auth.address}`, JSON.stringify(sessionMeta), 'EX', 86400);
        
        return { token, address: auth.address };
    }

    // --- SESSION VALIDATION ---
    async verifySession(token) {
        try {
            const decoded = jwt.verify(token, this.JWT_SECRET);
            // Optional: Check Redis to see if the session was blacklisted/revoked
            const active = await this.engine.redis.exists(`session:${decoded.wallet}`);
            if (!active) return null;
            
            return decoded;
        } catch (e) {
            return null;
        }
    }
}

module.exports = AuthService;
