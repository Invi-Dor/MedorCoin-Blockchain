// LOCATION: medorcoin-node/middleware/auth.cjs

import argon2 from "argon2";
import jwt from "jsonwebtoken";
import logger from "../utils/logger.cjs";

// 1. JWT SECRET VALIDATION (Area #1 Fix)
const JWT_SECRET = process.env.JWT_SECRET;
if (!JWT_SECRET) {
    logger.error("AUTH_FATAL", "JWT_SECRET is missing from environment. Exiting.");
    process.exit(1); 
}

// 2. ADJUSTABLE ARGON2 PARAMETERS (Area #2 Fix)
// For heavy load, reduce memoryCost to 2**14 (16MB).
const HASH_CONFIG = {
    type: argon2.argon2id,
    memoryCost: process.env.NODE_ENV === 'production' ? 2 ** 16 : 2 ** 14, 
    timeCost: 3,
    parallelism: 1
};

export async function hashPassword(password) {
    return await argon2.hash(password, HASH_CONFIG);
}

// 3. ACCOUNT EXISTENCE CHECK (Area #3 Fix)
export async function verifyUser(username, password, dbLookupFunc) {
    // PRE-CHECK: Identify user before verification to avoid unexpected throws
    const userAccount = await dbLookupFunc(username);
    
    if (!userAccount) {
        logger.warn("AUTH_FAIL", `Account not found for: ${username}`);
        throw new Error("Invalid Credentials"); // Obfuscated for security
    }

    const isValid = await argon2.verify(userAccount.passwordHash, password);
    if (!isValid) throw new Error("Invalid Credentials");
    
    return jwt.sign({ sub: username, role: userAccount.role }, JWT_SECRET, { expiresIn: '2h' });
}
