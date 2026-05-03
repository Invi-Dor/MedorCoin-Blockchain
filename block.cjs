"use strict";

const crypto = require('crypto');

/**
 * @class Block
 * @description 1:1 Bitcoin-standard 80-byte header with Recursive Merkle Tree.
 */
class Block {
    constructor(version, previousHash, transactions, bits, nonce = 0) {
        this.version = version;           // 4 Bytes
        this.previousHash = previousHash; // 32 Bytes
        this.timestamp = Math.floor(Date.now() / 1000); 
        this.bits = bits;                 // 4 Bytes (Target)
        this.nonce = nonce;               // 4 Bytes
        this.transactions = transactions; 
        
        // RECURSIVE MERKLE ROOT (Production Standard)
        this.merkleRoot = this.calculateMerkleRoot(); 
        
        // Exact 80-byte binary header for SHA-256d mining
        this.header = this.serializeHeader();
        this.hash = this.calculateHash();
    }

    /**
     * Exact 80-byte Header Construction
     * Matches Bitcoin Core memory layout
     */
    serializeHeader() {
        const buf = Buffer.alloc(80);
        
        buf.writeUInt32LE(this.version, 0);
        
        const prevBuf = Buffer.from(this.previousHash, 'hex');
        prevBuf.copy(buf, 4, 0, 32);
        
        const merkleBuf = Buffer.from(this.merkleRoot, 'hex');
        merkleBuf.copy(buf, 36, 0, 32);
        
        buf.writeUInt32LE(this.timestamp, 68);
        buf.writeUInt32LE(this.bits, 72);
        buf.writeUInt32LE(this.nonce, 76);
        
        return buf;
    }

    /**
     * SHA-256d (Double SHA-256)
     * High-speed hash of the 80-byte header only.
     */
    calculateHash() {
        const firstPass = crypto.createHash('sha256').update(this.header).digest();
        return crypto.createHash('sha256').update(firstPass).digest('hex');
    }

    /**
     * PRODUCTION RECURSIVE MERKLE TREE
     * Pairs hashes, duplicates odd nodes, reduces to single root.
     */
    calculateMerkleRoot() {
        if (!this.transactions || this.transactions.length === 0) {
            return "0".repeat(64);
        }

        // 1. Initial Leaf Hashes (Double-hash transactions)
        let level = this.transactions.map(tx => {
            const data = typeof tx === 'string' ? tx : JSON.stringify(tx);
            const first = crypto.createHash('sha256').update(data).digest();
            return crypto.createHash('sha256').update(first).digest();
        });

        // 2. Recursive reduction
        while (level.length > 1) {
            // Handle odd-numbered leaves by duplicating the last one
            if (level.length % 2 !== 0) {
                level.push(level[level.length - 1]);
            }

            const nextLevel = [];
            for (let i = 0; i < level.length; i += 2) {
                const combined = Buffer.concat([level[i], level[i + 1]]);
                const first = crypto.createHash('sha256').update(combined).digest();
                nextLevel.push(crypto.createHash('sha256').update(first).digest());
            }
            level = nextLevel;
        }

        return level[0].toString('hex');
    }

    /**
     * Basic structure validation
     */
    verify() {
        const checkHash = this.calculateHash();
        return checkHash === this.hash && this.header.length === 80;
    }

    static genesis() {
        return new Block(1, "0".repeat(64), [{ coinbase: "Medor Genesis 8888" }], 0x1d00ffff, 0);
    }
}

module.exports = Block;
