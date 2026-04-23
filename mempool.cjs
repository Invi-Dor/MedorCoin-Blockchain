const EventEmitter = require("events");
const logger = require("./logger");

class Mempool extends EventEmitter {
    constructor(utxoSet) {
        super();
        this.utxoSet = utxoSet;
        this.addon = null;
        this.queue = [];
        this.isProcessing = false;

        // INDUSTRIAL CONFIG
        this.MAX_QUEUE_SIZE = 10000;    // Risk #1 & #3 Fix: Prevents memory exhaustion
        this.EXECUTION_TIMEOUT = 2500;  // Risk #2 Fix: Prevents JS event-loop hang

        try {
            this.addon = require('./build/Release/medorcoin_addon.node');
            logger.info("MEMPOOL", "C++ Engine Linked: Hardened Boundary Active.");
        } catch (e) {
            // Risk #4 Fix: Fail-Fast (Loud Failure)
            logger.error("FATAL", "Consensus Addon Missing! Node cannot maintain integrity.");
            process.exit(1); 
        }
    }

    /**
     * PRODUCTION INGESTION
     * Implements Load-Shedding and Admission Control.
     */
    async addTransaction(tx, baseFee = 100) {
        // Risk #1 & #3: Early Rejection (Backpressure)
        if (this.queue.length >= this.MAX_QUEUE_SIZE) {
            return { ok: false, reason: "NODE_OVERLOADED_DROP_TX" };
        }

        return new Promise((resolve) => {
            this.queue.push({ tx, baseFee, resolve });
            this._runSequencer();
        });
    }

    async _runSequencer() {
        if (this.isProcessing || this.queue.length === 0) return;
        this.isProcessing = true;

        const { tx, baseFee, resolve } = this.queue.shift();

        // Risk #2: Timeout Protection (The Watchdog)
        const timeout = setTimeout(() => {
            logger.error("MEMPOOL", `Engine Stall on tx: ${tx.txHash}`);
            this.isProcessing = false;
            resolve({ ok: false, reason: "ENGINE_TIMEOUT_STALL" });
            setImmediate(() => this._runSequencer());
        }, this.EXECUTION_TIMEOUT);

        try {
            // CALL THE C++ ENGINE
            const result = await this.addon.mempool_addTransaction(tx, baseFee);
            
            clearTimeout(timeout); // Clear watchdog on success

            if (result && result.success) {
                this.emit("tx_added", tx.txHash);
                resolve({ ok: true, hash: tx.txHash });
            } else {
                resolve({ ok: false, reason: result?.error || "CONSENSUS_REJECTION" });
            }
        } catch (err) {
            clearTimeout(timeout);
            logger.error("MEMPOOL", `Fatal Engine Fault: ${err.message}`);
            resolve({ ok: false, reason: "ENGINE_CRITICAL_FAILURE" });
        } finally {
            this.isProcessing = false;
            setImmediate(() => this._runSequencer());
        }
    }

    getMiningTemplate(limit = 2000) {
        // Risk #4: Ensure we don't return silent empty arrays if addon is dead
        if (!this.addon) throw new Error("CRITICAL_NODE_FAILURE_ADDON_OFFLINE");
        return this.addon.mempool_getSortedEntries(limit);
    }
}

module.exports = Mempool;
