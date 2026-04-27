/**
 * FILE: medorcoin-node/services/db_service.cjs
 * PRODUCTION BUILD: Encrypted State Management with Brute-Force Protection
 */
import Redis from "ioredis";
import crypto from "crypto";
import logger from "../utils/logger.cjs";

const redis = new Redis(process.env.REDIS_URL || "redis://localhost:6379");

// CONFIGURATION
const ENCRYPTION_KEY = process.env.DB_ENCRYPTION_KEY; // Must be 32 chars in .env
const IV_LENGTH = 16;
const AUTH_CONFIG = {
    MAX_ATTEMPTS: 5,
    LOCKOUT_TIME: 900,      // 15 minutes
    CURRENT_HASH_VER: "v1" 
};

// --- ENCRYPTION HELPERS (Risk #4 Fix) ---

function encrypt(text) {
    if (!ENCRYPTION_KEY || ENCRYPTION_KEY.length !== 32) {
        throw new Error("DB_ENCRYPTION_KEY must be 32 characters.");
    }
    const iv = crypto.randomBytes(IV_LENGTH);
    const cipher = crypto.createCipheriv('aes-256-cbc', Buffer.from(ENCRYPTION_KEY), iv);
    let encrypted = cipher.update(text);
    encrypted = Buffer.concat([encrypted, cipher.final()]);
    return iv.toString('hex') + ':' + encrypted.toString('hex');
}

function decrypt(text) {
    const textParts = text.split(':');
    const iv = Buffer.from(textParts.shift(), 'hex');
    const encryptedText = Buffer.from(textParts.join(':'), 'hex');
    const decipher = crypto.createDecipheriv('aes-256-cbc', Buffer.from(ENCRYPTION_KEY), iv);
    let decrypted = decipher.update(encryptedText);
    decrypted = Buffer.concat([decrypted, decipher.final()]);
    return decrypted.toString();
}

// --- EXPORTED PRODUCTION METHODS ---

/**
 * The 'dbLookupFunc' required by auth.cjs
 * Decrypts data and checks for account lockouts.
 */
export async function findUserInDB(username) {
    const key = `user:${username}`;
    const lockKey = `lockout:${username}`;

    // 1. Brute-Force Protection Check (Risk #3)
    const attempts = await redis.get(lockKey);
    if (attempts && parseInt(attempts) >= AUTH_CONFIG.MAX_ATTEMPTS) {
        logger.warn("SECURITY", `Blocked access to locked account: ${username}`);
        throw new Error("ACCOUNT_LOCKED");
    }

    // 2. Retrieval & Decryption (Risk #4)
    const encryptedData = await redis.get(key);
    if (!encryptedData) return null;

    try {
        const decryptedBody = decrypt(encryptedData);
        const user = JSON.parse(decryptedBody);

        // 3. Versioning Metadata (Risk #1)
        if (user.version !== AUTH_CONFIG.CURRENT_HASH_VER) {
            user.requiresUpdate = true;
        }

        return user;
    } catch (err) {
        logger.error("DB_DECRYPT_FAIL", `Decryption failed for ${username}. Check ENCRYPTION_KEY.`);
        throw new Error("DATABASE_CORRUPTION_OR_KEY_MISMATCH");
    }
}

/**
 * Atomic User Save (Risk #5)
 * Encrypts user payload before committing to Redis.
 */
export async function saveUser(username, hash, role = "user") {
    const key = `user:${username}`;
    
    const userData = JSON.stringify({
        username,
        passwordHash: hash,
        role,
        version: AUTH_CONFIG.CURRENT_HASH_VER,
        createdAt: Date.now(),
        updatedAt: Date.now()
    });

    const ciphertext = encrypt(userData);

    try {
        // Multi/Exec for Atomicity
        await redis.multi()
            .set(key, ciphertext)
            .set(`user_status:${username}`, "active")
            .del(`lockout:${username}`) // Clear any old lockouts on successful re-save
            .exec();
            
        logger.info("DB", `Encrypted profile saved for user: ${username}`);
    } catch (err) {
        logger.error("DB_SAVE_FAIL", err.message);
        throw new Error("ATOMIC_SAVE_FAILURE");
    }
}

/**
 * Security: Track failed logins
 */
export async function trackLoginFailure(username) {
    const lockKey = `lockout:${username}`;
    const attempts = await redis.incr(lockKey);
    await redis.expire(lockKey, AUTH_CONFIG.LOCKOUT_TIME);
    return attempts;
}

/**
 * Security: Immediate Session Revocation (Risk #2)
 */
export async function invalidateSession(username) {
    await redis.set(`blacklist:${username}`, "true", "EX", 7200);
}
