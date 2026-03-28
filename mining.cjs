/**
 * MedorCoin - Production Mining Module (V6 Final - Binary Strict)
 * - Binary Parity: Fully aligned with C++ Merkle binary iterative folding.
 * - Endianness: Little-Endian byte-order for 256-bit target comparison.
 * - Logic: Real-time timestamp refreshing on nonce-wrap to prevent stale mining.
 */

const crypto = require("crypto");
const EventEmitter = require("events");
const logger = require("./logger");

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
        this.hashRate = 0n;
        this._statsTimer = null;
        this._abortController = null;
    }

    start() {
        if (this.mining) return;
        if (this.chain.isSyncing?.()) {
            logger.warn("MINER", "Chain sync in progress. Mining suspended.");
            return;
        }
        this.mining = true;
        this._statsTimer = setInterval(() => {
            this.hashRate = this._hashCount;
            this._hashCount = 0n;
        }, 1000);
        this._runMiningLoop();
    }

    stop() {
        this.mining = false;
        if (this._abortController) this._abortController.abort();
        if (this._statsTimer) clearInterval(this._statsTimer);
    }

    async _runMiningLoop() {
        while (this.mining) {
            this._abortController = new AbortController();
            try {
                const result = await this._mineNextBlock(this._abortController.signal);
                if (!result) await new Promise(r => setTimeout(r, 1000));
            } catch (err) {
                if (err.name !== 'AbortError') {
                    logger.error("MINER", `Mining Loop Error: ${err.message}`);
                    await new Promise(r => setTimeout(r, 2000));
                }
            }
        }
    }

    async _mineNextBlock(signal) {
        const tip = this.chain.getLatestBlock();
        const nextHeight = BigInt(tip.height + 1);
        const nBits = this.chain.calculateDifficulty(Number(nextHeight));
        const target = this._bitsToTarget(nBits);

        const shifts = nextHeight / HALVING_INTERVAL;
        const subsidy = shifts >= 64n ? 0n : (INITIAL_REWARD >> shifts);
        
        const { transactions, totalFees } = this._preparePayload();
        const coinbase = this._createCoinbase(subsidy + totalFees, Number(nextHeight));
        
        const allTxs = [coinbase, ...transactions.sort((a, b) => a.hash.localeCompare(b.hash))];
        const merkleRootHex = this._calculateMerkleRoot(allTxs);

        const header = Buffer.alloc(80);
        header.writeUInt32LE(1, 0); 
        
        const prevBuf = Buffer.from(tip.hash, 'hex');
        const rootBuf = Buffer.from(merkleRootHex, 'hex');
        if (prevBuf.length !== 32 || rootBuf.length !== 32) throw new Error("Consensus: Invalid Hash Size");
        
        prevBuf.copy(header, 4);
        rootBuf.copy(header, 36);
        header.writeUInt32LE(this._getValidatedTimestamp(tip.timestamp), 68);
        header.writeUInt32LE(nBits, 72);

        let nonce = 0n;
        logger.info("MINER", `Mining Block #${nextHeight} | Diff: ${nBits}`);

        while (!signal.aborted && this.mining) {
            nonce++;
            
            if (nonce > MAX_NONCE) {
                header.writeUInt32LE(this._getValidatedTimestamp(tip.timestamp), 68);
                nonce = 0n;
            }

            header.writeBigUInt64LE(nonce, 76);
            
            const hash = crypto.createHash("sha256").update(
                crypto.createHash("sha256").update(header).digest()
            ).digest();
            
            this._hashCount++;

            const hashInt = BigInt("0x" + Buffer.from(hash).reverse().toString('hex'));

            if (hashInt <= target) {
                const blockHash = Buffer.from(hash).reverse().toString('hex');
                const block = {
                    header: {
                        version: 1,
                        prevHash: tip.hash,
                        merkleRoot: merkleRootHex,
                        timestamp: header.readUInt32LE(68),
                        nBits: nBits,
                        nonce: nonce.toString()
                    },
                    transactions: allTxs,
                    hash: blockHash
                };
                return await this._submitBlock(block);
            }

            if (nonce % 20000n === 0n) {
                await new Promise(setImmediate);
                if (this.chain.getLatestBlock().hash !== tip.hash) return false;
            }
        }
        return false;
    }

    _preparePayload() {
        if (!this.mempool.getTransactionsForBlock) throw new Error("Mempool Interface Error");
        
        const txs = this.mempool.getTransactionsForBlock(MAX_BLOCK_WEIGHT) || [];
        const selected = [];
        let weight = 0;
        let fees = 0n;

        for (const tx of txs) {
            // Replaced JSON length check with rough binary size estimation
            const size = 16 + (tx.inputs.length * 180) + (tx.outputs.length * 34);
            if (size > 100000 || weight + size > MAX_BLOCK_WEIGHT - 4000) continue;
            selected.push(tx);
            weight += size;
            fees += BigInt(tx.fee || 0);
        }
        return { transactions: selected, totalFees: fees };
    }

    _createCoinbase(val, height) {
        // Pure UTXO coinbase. No Ethereum nonces.
        const tx = {
            version: 1,
            inputs: [{ txid: "0000000000000000000000000000000000000000000000000000000000000000", vout: 0xffffffff, scriptSig: `MedorNode:${height}:${Date.now()}` }],
            outputs: [{ address: this.wallet.address, value: val.toString() }],
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
                const hash = crypto.createHash("sha256").update(
                    crypto.createHash("sha256").update(combined).digest()
                ).digest();
                nextLevel.push(hash);
            }
            nodes = nextLevel;
        }
        return nodes[0].toString('hex');
    }

    _hashBinary(tx) {
        // Destroyed JSON.stringify. Strictly packs buffers.
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
        return crypto.createHash("sha256").update(
            crypto.createHash("sha256").update(raw).digest()
        ).digest("hex");
    }

    async _submitBlock(block) {
        try {
            const result = await this.chain.addBlock(block); // This will trigger _processBlock
            if (result.ok) {
                logger.info("MINER", `BLOCK ACCEPTED: ${block.hash}`);
                this.mempool.removeConfirmed?.(block.transactions.filter(t => t.inputs[0].vout !== 0xffffffff));
                await this._broadcastWithRetry(block);
                this.emit("block:mined", block);
                return true;
            }
        } catch (e) {
            logger.error("MINER", `Submission Failed: ${e.message}`);
        }
        return false;
    }

    async _broadcastWithRetry(block, attempts = 3) {
        for (let i = 0; i < attempts; i++) {
            try {
                await this.p2p.broadcast("block", block);
                return;
            } catch (e) {
                if (i === attempts - 1) logger.warn("P2P", "Block broadcast timed out.");
                await new Promise(r => setTimeout(r, 1000));
            }
        }
    }

    _bitsToTarget(bits) {
        const exp = (bits >> 24) & 0xff;
        const mant = bits & 0xffffff;
        return BigInt(mant) * (2n ** (8n * BigInt(exp - 3)));
    }

    _getValidatedTimestamp(prevTimestamp) {
        const now = Math.floor(Date.now() / 1000);
        const clamped = Math.max(prevTimestamp + 1, now);
        return (clamped > now + MAX_FUTURE_DRIFT_SEC) ? (now + MAX_FUTURE_DRIFT_SEC) : clamped;
    }
}

module.exports = Miner;
