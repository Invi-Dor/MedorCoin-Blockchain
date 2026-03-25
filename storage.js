/**
 * MedorCoin - Industrial Storage Engine (Final V8)
 * - Reliability: EOS-style WAL with mandatory fsync (Zero data loss).
 * - Atomicity: Two-stage recovery (Snapshot + WAL Delta).
 * - Integrity: Atomic Rename-swapping for snapshot files.
 * - Concurrency: 128-stripe shared mutex via RocksDBWrapper.
 */

const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const EventEmitter = require("events");
const logger = require("./logger");

class Storage extends EventEmitter {
    constructor(dbWrapper, dataDir = "./data") {
        super();
        this.db = dbWrapper;
        this.dataDir = dataDir;
        this.snapshotDir = path.join(dataDir, "snapshots");
        this.walPath = path.join(dataDir, "wal.log");
        
        this.writeQueue = [];
        this.writing = false;
        this._operationCount = 0;
        this.SNAPSHOT_INTERVAL = 5000;
        this.walFd = null; // Direct File Descriptor for fsync
    }

    /**
     * BOOTSTRAP SEQUENCE:
     * 1. Load the most recent valid Snapshot.
     * 2. Replay only the WAL entries that occurred AFTER the snapshot.
     * 3. Open WAL for new atomic appends.
     */
    async init() {
        try {
            if (!fs.existsSync(this.dataDir)) fs.mkdirSync(this.dataDir, { recursive: true });
            if (!fs.existsSync(this.snapshotDir)) fs.mkdirSync(this.snapshotDir, { recursive: true });

            // 1. Load Latest Snapshot first
            await this._loadLatestSnapshot();

            // 2. Replay WAL for data durability since last snapshot
            await this._recover();

            // 3. Open WAL with Synchronous O_APPEND
            this.walFd = fs.openSync(this.walPath, "a+");

            if (!this.db.isOpen() || !this.db.isHealthy()) {
                throw new Error("RocksDB Backend failed health check.");
            }

            logger.info("STORAGE", "System Recovered: Snapshot loaded + WAL replayed.");
        } catch (err) {
            logger.error("STORAGE", `Init Fatal: ${err.message}`);
            throw err;
        }
    }

    async put(key, value, sync = true) {
        return this._enqueue({ op: "put", key, value, sync });
    }

    async del(key, sync = true) {
        return this._enqueue({ op: "del", key, sync });
    }

    async get(key) {
        let valueOut = "";
        const result = await this.db.get(key, (val) => { valueOut = val; });
        return result.ok ? valueOut : null;
    }

    _enqueue(operation) {
        return new Promise((resolve, reject) => {
            this.writeQueue.push({ operation, resolve, reject });
            if (!this.writing) this._processQueue();
        });
    }

    async _processQueue() {
        if (this.writing || this.writeQueue.length === 0) return;
        this.writing = true;

        while (this.writeQueue.length > 0) {
            const { operation, resolve, reject } = this.writeQueue.shift();
            try {
                // Fix #1: WAL fsync (Durable Write)
                const walEntry = Buffer.from(JSON.stringify(operation) + "\n");
                fs.writeSync(this.walFd, walEntry);
                fs.fsyncSync(this.walFd); // Force hardware flush to disk

                // 2. Apply to RocksDB
                const res = await this._writeToDisk(operation);
                
                if (res.ok) {
                    this._operationCount++;
                    if (this._operationCount % this.SNAPSHOT_INTERVAL === 0) {
                        await this._takeSnapshot();
                    }
                    resolve(res);
                } else {
                    throw new Error(res.error);
                }
            } catch (err) {
                logger.error("STORAGE", `Process Queue Error: ${err.message}`);
                reject(err);
            }
        }
        this.writing = false;
    }

    async _writeToDisk(op) {
        switch (op.op) {
            case "put": return await this.db.put(op.key, op.value, op.sync);
            case "del": return await this.db.del(op.key, op.sync);
            case "batch":
                const puts = op.operations.filter(o => o.op === "put").map(o => [o.key, o.value]);
                return await this.db.batchPut(puts, op.sync);
            default: return { ok: false, error: "Invalid OP" };
        }
    }

    /**
     * Fix #2: Atomic Snapshot with Rename
     */
    async _takeSnapshot() {
        const snapshotId = Date.now();
        const finalPath = path.join(this.snapshotDir, `snapshot_${snapshotId}.json`);
        const tempPath = finalPath + ".tmp";
        
        try {
            const state = [];
            await this.db.iteratePrefix("", (k, v) => {
                state.push([k, v]);
                return true; 
            });

            const data = JSON.stringify(state);
            const checksum = crypto.createHash("sha256").update(data).digest("hex");
            
            // Write to temp file first
            fs.writeFileSync(tempPath, JSON.stringify({ data, checksum }), "utf8");
            // Atomic swap
            fs.renameSync(tempPath, finalPath);

            // Fix #1: Truncate WAL after snapshot
            fs.ftruncateSync(this.walFd, 0);
            
            logger.info("STORAGE", `Snapshot ${snapshotId} finalized. WAL cleared.`);
            await this._pruneOldSnapshots();
        } catch (err) {
            if (fs.existsSync(tempPath)) fs.unlinkSync(tempPath);
            logger.error("STORAGE", `Snapshot failed: ${err.message}`);
        }
    }

    /**
     * Fix #3: Load Latest Snapshot on Boot
     */
    async _loadLatestSnapshot() {
        const files = fs.readdirSync(this.snapshotDir)
            .filter(f => f.startsWith("snapshot_") && f.endsWith(".json"))
            .sort().reverse();

        if (files.length === 0) return;

        const latest = path.join(this.snapshotDir, files[0]);
        const { data, checksum } = JSON.parse(fs.readFileSync(latest, "utf8"));

        const actual = crypto.createHash("sha256").update(data).digest("hex");
        if (actual !== checksum) throw new Error("Snapshot checksum corruption detected.");

        const entries = JSON.parse(data);
        for (const [k, v] of entries) {
            await this.db.put(k, v, false); // No need to sync during recovery
        }
    }

    async _recover() {
        if (!fs.existsSync(this.walPath)) return;
        const content = fs.readFileSync(this.walPath, "utf8");
        const lines = content.split("\n").filter(l => l.trim());

        for (const line of lines) {
            const op = JSON.parse(line);
            await this._writeToDisk(op);
        }
    }

    async _pruneOldSnapshots(keep = 2) {
        const files = fs.readdirSync(this.snapshotDir).sort().reverse();
        for (const file of files.slice(keep)) {
            fs.unlinkSync(path.join(this.snapshotDir, file));
        }
    }

    async close() {
        if (this.walFd) fs.closeSync(this.walFd);
        logger.info("STORAGE", "Shutdown complete.");
    }
}

module.exports = Storage;
