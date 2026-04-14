"use strict";
const { Worker, isMainThread } = require("worker_threads");
const TransactionEngine = require('./transaction_engine.cjs');
const AuthService = require('./auth_service.cjs');

const engine = new TransactionEngine();
const authService = new AuthService(engine);
let addon = null; 

try {
    addon = require("./build/Release/medorcoin_addon.node");
} catch(e) { console.warn("Addon missing."); }

// Worker Logic and P2P logic stays here...

module.exports = { engine, authService, addon };


// ─── 1. HARDENED WORKER LOGIC ──────────────────────────────────
if (!isMainThread) {
    parentPort.on("message", ({ type, payload, id }) => {
        try {
            if (type === "verify_pow") {
                parentPort.postMessage({ id, result: _workerVerifyPoW(payload.block, payload.target) });
            } else if (type === "verify_tx") {
                parentPort.postMessage({ id, result: _workerVerifyTx(payload.tx, payload.utxoSnapshot) });
            }
        } catch (err) {
            parentPort.postMessage({ id, error: err.message });
        }
    });

    function _workerVerifyPoW(block, target) {
        const header = Buffer.alloc(80);
        header.writeUInt32LE(block.version, 0);
        Buffer.from(block.prevHash, "hex").copy(header, 4);
        Buffer.from(block.merkleRoot, "hex").copy(header, 36);
        header.writeUInt32LE(block.timestamp, 68);
        header.writeUInt32LE(block.nBits, 72);
        header.writeBigUInt64LE(BigInt(block.nonce), 76);
        
        const h = crypto.createHash("sha256").update(crypto.createHash("sha256").update(header).digest()).digest();
        return BigInt("0x" + h.reverse().toString("hex")) <= BigInt(target);
    }

    function _workerVerifyTx(tx, utxoSnapshot) {
        const txHash = crypto.createHash("sha256").update(JSON.stringify(tx.data)).digest();
        for (const input of tx.inputs) {
            const utxo = utxoSnapshot[`${input.txid}:${input.vout}`];
            if (!utxo || utxo.spent) return false;
            if (!secp256k1.ecdsaVerify(Buffer.from(input.signature, 'hex'), txHash, Buffer.from(input.publicKey, 'hex'))) return false;
        }
        return true;
    }
} else {
    // ─── 2. MAIN THREAD & FULL WORKERPOOL ────────────────────────
    class WorkerPool {
        constructor(size) {
            this.size = size;
            this.workers = [];
            this.queue = [];
            this._pending = new Map();
            this._seq = 0;
        }
        init() {
            for (let i = 0; i < this.size; i++) {
                const w = new Worker(__filename);
                w.on("message", ({ id, result, error }) => {
                    const handler = this._pending.get(id);
                    if (!handler) return;
                    this._pending.delete(id);
                    error ? handler.reject(new Error(error)) : handler.resolve(result);
                    this._next(w);
                });
                w.on("error", (err) => console.error(`Worker Error: ${err.message}`));
                this.workers.push(w);
            }
        }
        _next(worker) {
            worker._busy = false;
            if (this.queue.length > 0) {
                const { msg, resolve, reject } = this.queue.shift();
                this._dispatch(worker, msg, resolve, reject);
            }
        }
        run(type, payload) {
            return new Promise((resolve, reject) => {
                const free = this.workers.find(w => !w._busy);
                const msg = { type, payload, id: ++this._seq };
                free ? this._dispatch(free, msg, resolve, reject) : this.queue.push({ msg, resolve, reject });
            });
        }
        _dispatch(worker, msg, resolve, reject) {
            worker._busy = true;
            this._pending.set(msg.id, { resolve, reject });
            worker.postMessage(msg);
        }
        terminate() { this.workers.forEach(w => w.terminate()); }
    }

    // ─── 3. INFRASTRUCTURE & CONSENSUS ──────────────────────────
    const { server, app, engine, addon } = require("./server.cjs");
    const P2PNetwork = require("./p2p_network.cjs");
    const pool = new WorkerPool(os.cpus().length);
    pool.init();

    const p2p = new P2PNetwork(engine, { port: process.env.P2P_PORT || 8333 });
    const scorecard = new Map(); // Peer ID -> { score, latency }

    // Difficulty Adjustment Logic
    function getTarget(nBits) {
        const exp = (nBits >> 24) & 0xff;
        const mant = nBits & 0xffffff;
        return BigInt(mant) * (2n ** (8n * BigInt(exp - 3)));
    }

    // CONSENSUS GATEWAY
    async function validateBlock(block, peerId) {
        // 1. Basic Consensus Checks
        if (block.timestamp > (Date.now() / 1000) + 7200) return false; // Too far in future
        if (block.height > 0 && !block.prevHash) return false;

        // 2. PoW & Difficulty Check
        const target = getTarget(block.nBits);
        const isValid = await pool.run("verify_pow", { block, target: target.toString() });
        
        if (isValid) {
            await engine.applyBlock(block);
            scorecard.set(peerId, (scorecard.get(peerId) || 100) + 10);
            p2p.broadcast("block", block, peerId);
        } else {
            p2p.penalizePeer(peerId, 20); // Scorecard enforcement
        }
    }

    // ─── 4. MONITORING & DISPATCH ──────────────────────────────
    setInterval(() => {
        const stats = {
            mempool: engine.mempoolSize,
            peers: p2p.peers.size,
            poolQueue: pool.queue.length,
            uptime: process.uptime()
        };
        console.log(`[HEALTH] ${JSON.stringify(stats)}`);
    }, 30000);

    app.post("/tx", async (req, res) => {
        const isValid = await pool.run("verify_tx", { tx: req.body, utxoSnapshot: await engine.getSnapshot(req.body) });
        if (isValid) {
            p2p.broadcast("tx", req.body);
            addon.submitTransaction(JSON.stringify(req.body), (err) => res.json({ success: !err }));
        } else {
            res.status(400).json({ error: "Consensus failure: Signature/UTXO invalid" });
        }
    });

    p2p.on("block", validateBlock);
    p2p.start();
    server.listen(process.env.PORT || 5001, '0.0.0.0');
}
