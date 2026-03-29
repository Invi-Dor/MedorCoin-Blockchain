/**
 * MedorCoin - Production Mining Module (V8 Industrial Cluster-Ready)
 * - Binary Parity: Fully aligned with C++ Merkle binary iterative folding.
 * - Cluster Integration: Reports metrics and blocks to the Master.
 * - Resilience: Implements Task Queueing (max 10) and Spillover protection.
 */

const crypto = require("crypto");
const EventEmitter = require("events");
const logger = require("./log_transport.cjs");

const MAX_BLOCK_WEIGHT = 1000000;
const INITIAL_REWARD = 5000000000n;
const HALVING_INTERVAL = 210000n;
const MAX_NONCE = 0xFFFFFFFFFFFFFFFFn;
const MAX_FUTURE_DRIFT_SEC = 7200;

class Miner extends EventEmitter {
    constructor(mempool, chain, wallet, p2p) {
        super();
        this.mempool = mempool;
        this.chain = chain;
        this.wallet = wallet;
        this.p2p = p2p;

        this.mining = false;
        this._hashCount = 0n;
        this._abortController = null;

        // PROBLEM FIX 3: Worker Task Queue (Adaptive Buffering)
        this.taskQueue = [];
        this.MAX_TASK_QUEUE = 10;

        // PROBLEM FIX 1: Two-Tier Submission (Worker Side)
        this.submissionQueue = [];
        this.spilloverBuffer = [];
        this.MAX_PRIMARY_QUEUE = 50;
    }

    /**
     * Entry point for Cluster Workers
     * Listens for NEW_WORK from mining_master.cjs
     */
    initWorkerMode() {
        process.on('message', (msg) => {
            if (msg.type === 'NEW_WORK') {
                this.taskQueue.push(msg.workLoad);
                if (this.taskQueue.length > this.MAX_TASK_QUEUE) this.taskQueue.shift();
                this._processNextTask();
            }
        });

        // PROBLEM FIX 4: Metric Reporting to Master
        setInterval(() => {
            if (this._hashCount > 0n) {
                process.send({ type: 'METRICS', hashes: this._hashCount.toString() });
                this._hashCount = 0n;
            }
        }, 1000);
    }

    async _processNextTask() {
        if (this.taskQueue.length === 0) return;

        const workLoad = this.taskQueue.pop();
        this.taskQueue.length = 0; // Clear stale work

        if (this._abortController) this._abortController.abort();
        this._abortController = new AbortController();

        try {
            const block = await this._mineWork(workLoad, this._abortController.signal);
            if (block) await this._submitToMaster(block);
        } catch (e) {
            if (e.name !== 'AbortError') {
                process.send({ type: 'ERROR', error: e.message });
            }
        }
    }

    async _mineWork(work, signal) {
        const { previousHash, difficulty, minerAddress, height, mempool } = work;
        const target = this._bitsToTarget(Number(difficulty));

        const coinbase = this._createCoinbase(this._calculateSubsidy(BigInt(height)), height, minerAddress);
        const allTxs = [coinbase, ...mempool.sort((a, b) => a.hash.localeCompare(b.hash))];
        const merkleRootHex = this._calculateMerkleRoot(allTxs);

        const header = Buffer.alloc(80);
        header.writeUInt32LE(1, 0);
        Buffer.from(previousHash, 'hex').copy(header, 4);
        Buffer.from(merkleRootHex, 'hex').copy(header, 36);
        header.writeUInt32LE(Math.floor(Date.now() / 1000), 68);
        header.writeUInt32LE(Number(difficulty), 72);

        let nonce = 0n;
        while (!signal.aborted) {
            nonce++;
            if (nonce > MAX_NONCE) return null;

            header.writeBigUInt64LE(nonce, 76);
            const hash = crypto.createHash("sha256").update(
                crypto.createHash("sha256").update(header).digest()
            ).digest();

            this._hashCount++;

            const hashInt = BigInt("0x" + Buffer.from(hash).reverse().toString('hex'));
            if (hashInt <= target) {
                return {
                    header: {
                        version: 1,
                        prevHash: previousHash,
                        merkleRoot: merkleRootHex,
                        timestamp: header.readUInt32LE(68),
                        nBits: Number(difficulty),
                        nonce: nonce.toString()
                    },
                    transactions: allTxs,
                    hash: Buffer.from(hash).reverse().toString('hex'),
                    minerAddress // PROBLEM FIX 2: Explicitly pass for fallback tracking
                };
            }

            if (nonce % 50000n === 0n) await new Promise(setImmediate);
        }
        return null;
    }

    // PROBLEM FIX 1: Submission with Spillover Logic
    async _submitToMaster(block) {
        if (this.submissionQueue.length >= this.MAX_PRIMARY_QUEUE) {
            if (this.spilloverBuffer.length < 100) {
                this.spilloverBuffer.push(block);
                logger.shipToTransport("WARN", "MINER", "Primary submission queue full. Block moved to spillover.");
            } else {
                logger.shipToTransport("CRITICAL", "MINER", "Buffers exhausted. Block dropped.");
                return;
            }
        } else {
            this.submissionQueue.push(block);
        }

        // Send to Master Process
        process.send({
            type: 'BLOCK_FOUND',
            block: block,
            mempool: block.transactions
        });
    }

    _calculateSubsidy(height) {
        const shifts = height / HALVING_INTERVAL;
        return shifts >= 64n ? 0n : (INITIAL_REWARD >> shifts);
    }

    _createCoinbase(val, height, minerAddress) {
        const tx = {
            version: 1,
            inputs: [{
                txid: "0000000000000000000000000000000000000000000000000000000000000000",
                vout: 0xffffffff,
                scriptSig: `MedorNode:${height}:${Date.now()}`
            }],
            outputs: [{ address: minerAddress, value: val.toString() }],
            locktime: 0
        };
        tx.hash = this._hashBinary(tx);
        return tx;
    }

    _calculateMerkleRoot(txs) {
        let nodes = txs.map(tx => Buffer.from(tx.hash, 'hex'));
        if (nodes.length === 0) return Buffer.alloc(32).toString('hex');
        while (nodes.length > 1) {
            const nextLevel = [];
            for (let i = 0; i < nodes.length; i += 2) {
                const left = nodes[i];
                const right = (i + 1 < nodes.length) ? nodes[i + 1] : left;
                const combined = Buffer.concat([left, right]);
                const hash = crypto.createHash("sha256").update(crypto.createHash("sha256").update(combined).digest()).digest();
                nextLevel.push(hash);
            }
            nodes = nextLevel;
        }
        return nodes[0].toString('hex');
    }

    _hashBinary(tx) {
        const header = Buffer.alloc(16);
        header.writeUInt32LE(tx.version || 1, 0);
        header.writeUInt32LE(tx.inputs.length, 4);
        header.writeUInt32LE(tx.outputs.length, 8);
        header.writeUInt32LE(tx.locktime || 0, 12);
        const bufs = [header];
        for (const i of tx.inputs) {
            const b = Buffer.alloc(36);
            Buffer.from(i.txid, 'hex').copy(b, 0);
            b.writeUInt32LE(i.vout, 32);
            bufs.push(b);
        }
        for (const o of tx.outputs) {
            const b = Buffer.alloc(8);
            b.writeBigUInt64LE(BigInt(o.value), 0);
            bufs.push(b);
        }
        const raw = Buffer.concat(bufs);
        return crypto.createHash("sha256").update(crypto.createHash("sha256").update(raw).digest()).digest("hex");
    }

    _bitsToTarget(bits) {
        const exp = (bits >> 24) & 0xff;
        const mant = bits & 0xffffff;
        return BigInt(mant) * (2n ** (8n * BigInt(exp - 3)));
    }
}

module.exports = Miner;
