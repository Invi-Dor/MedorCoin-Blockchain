const { ethers } = require('ethers');
const argon2 = require('argon2');
const jwt = require('jsonwebtoken');
const pino = require('pino');

// Match your logging style
const logger = pino({
    level: process.env.LOG_LEVEL || 'info',
    base: { service: 'auth-provider' },
    transport: { target: 'pino-pretty' }
});

class AuthService {
    constructor(engine) {
        this.engine = engine;
        this.JWT_SECRET = process.env.JWT_SECRET;
        
        if (!this.JWT_SECRET) {
            logger.warn("No JWT_SECRET detected. Using unsafe emergency fallback.");
            this.JWT_SECRET = 'emergency_medor_secret_99';
        }
    }

    async signup(username, password) {
        const start = Date.now();
        try {
            // Check existence using the engine's high-speed cluster connection
            const existing = await this.engine.cluster.get(`auth:${username}`);
            if (existing) throw new Error("ID_CLAIMED");

            const wallet = ethers.Wallet.createRandom();
            const passwordHash = await argon2.hash(password, {
                type: argon2.argon2id,
                memoryCost: 2 ** 16,
                timeCost: 3
            });

            const userData = {
                username,
                address: wallet.address,
                passwordHash,
                // We keep mnemonic handling strictly for the user's initial response
                createdAt: Date.now()
            };

            // Atomic Multi-set: Save user and Init balance in one pipeline
            const pipeline = this.engine.cluster.pipeline();
            pipeline.set(`auth:${username}`, JSON.stringify(userData));
            pipeline.hset("{mdc}:balances", wallet.address, "0");
            await pipeline.exec();

            logger.info({ 
                user: username, 
                address: wallet.address, 
                latency: `${Date.now() - start}ms` 
            }, "New Identity Anchored to Medor State");

            return { username, address: wallet.address, mnemonic: wallet.mnemonic.phrase };
        } catch (e) {
            logger.error({ err: e.message }, "Provisioning Failed");
            throw e;
        }
    }

    // ... login methods follow same pattern
}

module.exports = AuthService;
