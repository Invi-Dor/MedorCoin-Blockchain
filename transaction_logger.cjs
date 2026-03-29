/**
 * MEDORCOIN ENGINE - INDUSTRIAL CORE (transaction_engine.cjs)
 * Final Resolution: Task Serialization, State Root Integrity, 
 * Full Reorg Rollbacks, and Integrity-Verified Crash Recovery.
 */

const fs = require('fs').promises;
const fssync = require('fs');
const path = require('path');
const crypto = require('crypto');
const zlib = require('zlib');
const { Level } = require('level');
const Merkle = require('./merkle.cjs');
const logger = require('./log_transport.cjs');

class TransactionEngine {
    constructor(options = {}) {
        this.nodeId = options.nodeId || "medor-industrial-01";
        this.dbPath = options.dbPath || path.join(__dirname, `db/ledger_${this.nodeId}`);
        this.walPath = options.walPath || path.join(__dirname, `db/wal_${this.nodeId}.dat`);
        this.archiveDir = path.join(__dirname, 'db/archives');
        this.hmacSecret = options.hmacSecret || "medor_internal_integrity_2026";

        this.db = new Level(this.dbPath, { valueEncoding: 'json' });

        this.PARTITIONS = {
            MEMPOOL: 'm:',
            CONFIRMED: 'c:',
            STATE: 's:',      
            BLOCK: 'b:',      
            INDEX: 'i:',      
            HISTORY: 'h:',    // Undo Logs
            PENDING: 'p:'     // Pending Balances
        };

        // 1. SERIALIZED QUEUE: True FIFO to prevent concurrent race conditions
        this.queue = Promise.resolve();
        
        // 5. WAL: High-speed async stream
        this.walStream = fssync.createWriteStream(this.walPath, { flags: 'a' });
        
        this.currentHeight = 0;
        this.lastBlockHash = "0".repeat(64);
        this.lastStateRoot = "0".repeat(64);

        if (!fssync.existsSync(this.archiveDir)) fssync.mkdirSync(this.archiveDir, { recursive: true });
    }

    /**
     * 1. CONCURRENCY RESOLUTION
     * Replaces array-based queue with a Promise chain to guarantee zero race conditions.
     */
    async _enqueue(task) {
        this.queue = this.queue.then(async () => {
            try { return await task(); } 
            catch (e) { logger.shipToTransport("ERROR", "ENGINE", e.message); throw e; }
        });
        return this.queue;
    }

    /**
     * 4. STATE ROOT CALCULATION
     * Computes a hash of the entire state to guarantee chain integrity.
     */
    async _calculateStateRoot() {
        const states = [];
        for await (const [key, value] of this.db.iterator({ gte: this.PARTITIONS.STATE, lte: this.PARTITIONS.STATE + '\xff' })) {
            states.push(`${key}:${value.balance}`);
        }
        return crypto.createHash('sha256').update(states.join('|')).digest('hex');
    }

    /**
     * 7. CRASH RECOVERY
     * Replays WAL and verifies HMAC integrity on boot.
     */
    async recoverFromCrash() {
        return this._enqueue(async () => {
            if (!fssync.existsSync(this.walPath)) return;
            const raw = fssync.readFileSync(this.walPath, 'utf8');
            const lines = raw.split('\n').filter(Boolean);
            
            for (const line of lines) {
                const [sig, json] = line.split(/:(.+)/);
                const check = crypto.createHmac('sha256', this.hmacSecret).update(json).digest('hex');
                if (sig === check) {
                    const tx = JSON.parse(json);
                    await this._stageInternal(tx); // Bypass duplicate WAL write
                }
            }
            fssync.truncateSync(this.walPath, 0); // Clear WAL after recovery
            logger.shipToTransport("INFO", "SYSTEM", "Crash Recovery Successful");
        });
    }

    async stageToMempool(tx) {
        return this._enqueue(async () => {
            const txHash = await this._stageInternal(tx);
            await this._writeToWAL({ ...tx, _sig: txHash });
            return txHash;
        });
    }

    async _stageInternal(tx) {
        const txHash = crypto.createHash('sha256').update(JSON.stringify(tx)).digest('hex');
        const cost = BigInt(tx.value) + (21000n + BigInt(JSON.stringify(tx).length) * 16n);

        const state = await this.db.get(`${this.PARTITIONS.STATE}${tx.from}`).catch(() => ({ balance: "0" }));
        const pending = await this.db.get(`${this.PARTITIONS.PENDING}${tx.from}`).catch(() => ({ total: "0" }));
        
        if (BigInt(state.balance) - BigInt(pending.total) < cost && tx.from !== '0x00') {
            throw new Error("Insufficient Funds for TX + Gas");
        }

        const batch = this.db.batch();
        batch.put(`${this.PARTITIONS.PENDING}${tx.from}`, { total: (BigInt(pending.total) + cost).toString() });
        batch.put(`${this.PARTITIONS.MEMPOOL}${txHash}`, { ...tx, _sig: txHash, gasUsed: (cost - BigInt(tx.value)).toString() });
        await batch.write();
        return txHash;
    }

    /**
     * 6. REORG / ROLLBACK SUPPORT
     * Uses the Undo Log to revert states if a fork is detected.
     */
    async rollbackBlock(height) {
        return this._enqueue(async () => {
            const undoLog = await this.db.get(`${this.PARTITIONS.HISTORY}${height}`).catch(() => null);
            if (!undoLog) return;

            const batch = this.db.batch();
            for (const entry of undoLog) {
                batch.put(`${this.PARTITIONS.STATE}${entry.address}`, { balance: entry.balance });
            }

            const txHashes = await this.db.get(`${this.PARTITIONS.INDEX}${height}`);
            for (const hash of txHashes) {
                const tx = await this.db.get(`${this.PARTITIONS.CONFIRMED}${hash}`);
                batch.put(`${this.PARTITIONS.MEMPOOL}${hash}`, { ...tx, status: 'unconfirmed' });
                batch.del(`${this.PARTITIONS.CONFIRMED}${hash}`);
            }

            batch.del(`${this.PARTITIONS.BLOCK}${height}`).del(`${this.PARTITIONS.INDEX}${height}`).del(`${this.PARTITIONS.HISTORY}${height}`);
            await batch.write();

            const prev = await this.db.get(`${this.PARTITIONS.BLOCK}${height - 1}`).catch(() => ({ hash: "0".repeat(64), stateRoot: "0".repeat(64) }));
            this.currentHeight = height - 1;
            this.lastBlockHash = prev.hash;
            this.lastStateRoot = prev.stateRoot;
        });
    }

    async confirmBlock(transactions, miner, hash, height) {
        return this._enqueue(async () => {
            const batch = this.db.batch();
            const txHashes = transactions.map(t => t._sig);
            const undoLog = [];
            let totalFees = 0n;

            for (const tx of transactions) {
                totalFees += BigInt(tx.gasUsed);
                undoLog.push(await this._getUndoEntry(tx.from), await this._getUndoEntry(tx.to));
                
                await this._mutateBalance(batch, tx.from, (BigInt(tx.value) + BigInt(tx.gasUsed)), false);
                await this._mutateBalance(batch, tx.to, BigInt(tx.value), true);
                
                const p = await this.db.get(`${this.PARTITIONS.PENDING}${tx.from}`).catch(() => ({ total: "0" }));
                const remains = BigInt(p.total) - (BigInt(tx.value) + BigInt(tx.gasUsed));
                
                // 2. PENDING KEY HANDLING: Ensure keys exist for consistent code expectations
                batch.put(`${this.PARTITIONS.PENDING}${tx.from}`, { total: (remains > 0n ? remains.toString() : "0") });
                batch.del(`${this.PARTITIONS.MEMPOOL}${tx._sig}`).put(`${this.PARTITIONS.CONFIRMED}${tx._sig}`, { ...tx, height, status: 'confirmed' });
            }

            undoLog.push(await this._getUndoEntry(miner));
            await this._mutateBalance(batch, miner, (totalFees + 5000000000n), true);
            
            await batch.write(); // Commit state before root calculation
            const stateRoot = await this._calculateStateRoot();

            const finalBatch = this.db.batch();
            finalBatch.put(`${this.PARTITIONS.BLOCK}${height}`, { hash, merkleRoot: Merkle.computeRoot(txHashes), stateRoot, prevHash: this.lastBlockHash });
            finalBatch.put(`${this.PARTITIONS.INDEX}${height}`, txHashes);
            finalBatch.put(`${this.PARTITIONS.HISTORY}${height}`, undoLog);
            await finalBatch.write();

            this.currentHeight = height;
            this.lastBlockHash = hash;
            this.lastStateRoot = stateRoot;
        });
    }

    /**
     * 3. STREAMING SNAPSHOT (Industrial Gzip)
     * Guaranteed valid JSON even with missing blocks.
     */
    async createSnapshot(start, end, prune = true) {
        const snapPath = path.join(this.archiveDir, `snap_${start}_${end}.json.gz`);
        const output = fssync.createWriteStream(snapPath);
        const compress = zlib.createGzip();
        compress.pipe(output);

        compress.write('{"blocks":[');
        let first = true;
        const pruneBatch = this.db.batch();

        for (let h = start; h <= end; h++) {
            const header = await this.db.get(`${this.PARTITIONS.BLOCK}${h}`).catch(() => null);
            if (!header) continue; // Skip missing blocks safely

            const txHashes = await this.db.get(`${this.PARTITIONS.INDEX}${h}`);
            const txs = await Promise.all(txHashes.map(hash => this.db.get(`${this.PARTITIONS.CONFIRMED}${hash}`)));

            if (!first) compress.write(',');
            compress.write(JSON.stringify({ height: h, header, transactions: txs }));
            first = false;

            if (prune) {
                pruneBatch.del(`${this.PARTITIONS.BLOCK}${h}`).del(`${this.PARTITIONS.INDEX}${h}`).del(`${this.PARTITIONS.HISTORY}${h}`);
                txHashes.forEach(hash => pruneBatch.del(`${this.PARTITIONS.CONFIRMED}${hash}`));
            }
        }

        compress.write(']}');
        compress.end();

        return new Promise(resolve => output.on('finish', async () => {
            if (prune) await pruneBatch.write();
            resolve(snapPath);
        }));
    }

    async _writeToWAL(entry) {
        const data = JSON.stringify(entry);
        const sig = crypto.createHmac('sha256', this.hmacSecret).update(data).digest('hex');
        if (!this.walStream.write(`${sig}:${data}\n`)) {
            await new Promise(r => this.walStream.once('drain', r));
        }
    }

    async _mutateBalance(batch, address, amount, isCredit) {
        const key = `${this.PARTITIONS.STATE}${address}`;
        const state = await this.db.get(key).catch(() => ({ balance: "0" }));
        const next = isCredit ? BigInt(state.balance) + amount : BigInt(state.balance) - amount;
        batch.put(key, { balance: next.toString() });
    }

    async _getUndoEntry(address) {
        const s = await this.db.get(`${this.PARTITIONS.STATE}${address}`).catch(() => ({ balance: "0" }));
        return { address, balance: s.balance };
    }

    shutdown() {
        this.walStream.end();
        this.db.close();
    }
}

module.exports = TransactionEngine;
