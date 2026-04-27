/**
 * FILE: medorcoin-node/services/db_service.cjs
 * INDUSTRIAL VERSION: High-Concurrency & Secure State Management
 */
import Redis from "ioredis";
import logger from "../utils/logger.cjs";

const redis = new Redis(process.env.REDIS_URL || "redis://localhost:6379");

// CONFIG FOR PRODUCTION HARDENING
const AUTH_CONFIG = {
    MAX_ATTEMPTS: 5,        // Risk #3: Brute-force cap
    LOCKOUT_TIME: 900,      // 15 minutes in seconds
    CURRENT_HASH_VER: "v1"  // Risk #1: Hash versioning
};

/**
 * The 'dbLookupFunc' required by auth.cjs
 * Includes Brute-Force Check & Versioning Metadata
 */
export async function findUserInDB(username) {
    const key = `user:${username}`;
    const lockKey = `lockout:${username}`;

    // 1. BRUTE FORCE CHECK (Risk #3)
    const attempts = await redis.get(lockKey);
    if (attempts && parseInt(attempts) >= AUTH_CONFIG.MAX_ATTEMPTS) {
        logger.warn("SECURITY", `Blocked login attempt for locked account: ${username}`);
        throw new Error("ACCOUNT_LOCKED_TOO_MANY_ATTEMPTS");
    }

    const data = await redis.get(key);
    if (!data) return null;

    const user = JSON.parse(data);
    
    // 2. HASH VERSIONING CHECK (Risk #1)
    if (user.version !== AUTH_CONFIG.CURRENT_HASH_VER) {
        logger.info("MAINTENANCE", `User ${username} flagged for hash migration.`);
        user.requiresUpdate = true; 
    }

    return user;
}

/**
 * ATOMIC USER REGISTRATION (Risk #5)
 * Uses Redis Transaction to ensure data integrity
 */
export async function saveUser(username, hash, role = "user") {
    const key = `user:${username}`;
    
    const userData = JSON.stringify({
        username,
        passwordHash: hash,
        role,
        version: AUTH_CONFIG.CURRENT_HASH_VER, // Risk #1 fix
        createdAt: Date.now(),
        updatedAt: Date.now()
    });

    try {
        // Multi/Exec ensures the set happens atomically
        const result = await redis.multi()
            .set(key, userData)
            .set(`user_status:${username}`, "active")
            .exec();
        
        return result;
    } catch (err) {
        logger.error("DB_ERROR", `Failed atomic save for ${username}: ${err.message}`);
        throw new Error("TRANSACTION_SAVE_FAILURE");
    }
}

/**
 * BRUTE FORCE INCREMENTOR (Risk #3 Fix)
 */
export async function trackLoginFailure(username) {
    const lockKey = `lockout:${username}`;
    await redis.incr(lockKey);
    await redis.expire(lockKey, AUTH_CONFIG.LOCKOUT_TIME);
}

/**
 * SESSION INVALIDATION (Risk #2 Fix)
 */
export async function invalidateSession(username) {
    // This allows us to "blacklist" a JWT before it expires
    await redis.set(`blacklist:${username}`, "true", "EX", 7200); // 2 hour TTL
}
