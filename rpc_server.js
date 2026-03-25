/**
 * MedorCoin Industrial Gateway (V10 - Production)
 * - Memory-Cached UI: miners.html is cached in a Buffer to prevent Disk I/O bottlenecks.
 * - Non-Blocking Architecture: Uses Async/Await for all RocksDB and P2P calls.
 * - Fault Tolerant: Implements full JSON-RPC 2.0 error specs and 413 Entity handling.
 * - Resource Shield: 5-second request timeouts and strict body-size enforcement.
 */

const http = require("http");
const fs = require("fs");
const path = require("path");
const logger = require("./logger");

class RPCServer {
    static PORT = 8332;
    static MAX_BODY_SIZE = 256 * 1024; // 256KB limit
    static REQUEST_TIMEOUT = 5000;      // 5s timeout to prevent Slowloris attacks

    constructor(chain, mempool, miner, p2p) {
        this.chain = chain;
        this.mempool = mempool;
        this.miner = miner;
        this.p2p = p2p;
        
        // Load UI into memory once at startup for "infinite" user speed
        this.uiCache = null;
        this._initCache();
        
        this.methods = this._registerMethods();
    }

    _initCache() {
        try {
            const uiPath = path.resolve(__dirname, "miners.html");
            this.uiCache = fs.readFileSync(uiPath);
            logger.info("RPC", "UI Cache Initialized: miners.html loaded to memory.");
        } catch (e) {
            logger.error("RPC", "Failed to cache miners.html! Ensure file exists in root.");
        }
    }

    _registerMethods() {
        return {
            "web3_clientversion": async () => "MedorCoin/V1.0.0/Industrial",
            "net_peercount":      async () => `0x${this.p2p.peers.size.toString(16)}`,
            "eth_blocknumber":    async () => `0x${this.chain.getTip().height.toString(16)}`,
            
            // Industrial Mining Continuity Logic
            "getuserstats": async ([address]) => {
                const balance = await this.chain.utxoSet.getBalance(address);
                const stats = this.miner.getStats();
                return {
                    address,
                    balance,
                    globalHashrate: stats.hashrate,
                    difficulty: stats.difficulty,
                    height: this.chain.getTip().height
                };
            },
            
            "sendrawtransaction": async (params) => await this._handleSendRawTx(params),
            "getmininginfo":      async () => this.miner.getStats()
        };
    }

    start() {
        this.server = http.createServer((req, res) => {
            // High-Performance Static Routing
            if (req.method === "GET" && (req.url === "/" || req.url === "/index.html")) {
                return this._serveUI(res);
            }

            if (req.method === "POST") {
                return this._onRequest(req, res);
            }

            this._sendRaw(res, 405, "Method Not Allowed");
        });

        // Industrial Error Handling for the Server Instance
        this.server.on("error", (e) => logger.error("RPC_SVR", e.message));
        this.server.listen(RPCServer.PORT, () => {
            logger.info("RPC", `Industrial Gateway Online at Port ${RPCServer.PORT}`);
        });
    }

    _serveUI(res) {
        if (!this.uiCache) return this._sendRaw(res, 500, "UI Template Missing");
        res.writeHead(200, { 
            "Content-Type": "text/html",
            "Cache-Control": "public, max-age=3600" 
        });
        res.end(this.uiCache);
    }

    async _onRequest(req, res) {
        try {
            const body = await this._readBody(req, res);
            if (!body) return; // Already handled by _readBody error logic

            const rpc = JSON.parse(body);
            
            // Batch processing support
            if (Array.isArray(rpc)) {
                const results = await Promise.all(rpc.map(q => this._dispatch(q)));
                return this._send(res, results);
            }

            const result = await this._dispatch(rpc);
            this._send(res, result);

        } catch (err) {
            this._sendError(res, -32700, "Parse error", null);
        }
    }

    async _dispatch(rpc) {
        const { method, params, id } = rpc;
        if (!method) return { jsonrpc: "2.0", error: { code: -32600, message: "Invalid Request" }, id };

        // Normalizing Method names to prevent casing failures
        const handler = this.methods[method.toLowerCase()];
        if (!handler) return { jsonrpc: "2.0", error: { code: -32601, message: "Method not found" }, id };

        try {
            const result = await handler(params || []);
            return { jsonrpc: "2.0", result, id };
        } catch (err) {
            return { jsonrpc: "2.0", error: { code: err.code || -32603, message: err.message }, id };
        }
    }

    // --- FIX: Logic Handlers Added ---
    async _handleSendRawTx([rawTx]) {
        if (!rawTx) throw { code: -32602, message: "Raw transaction hex required" };
        const res = await this.mempool.addTransaction(rawTx);
        if (!res.ok) throw { code: -1, message: res.reason };
        
        // Broadcast to P2P network immediately
        this.p2p.broadcast("tx", rawTx);
        return res.txid;
    }

    // --- FIX: Robust Body Reader with Timeout and 413 ---
    _readBody(req, res) {
        return new Promise((resolve) => {
            let data = "";
            const timer = setTimeout(() => {
                req.destroy();
                this._sendRaw(res, 408, "Request Timeout");
                resolve(null);
            }, RPCServer.REQUEST_TIMEOUT);

            req.on("data", (chunk) => {
                data += chunk;
                if (data.length > RPCServer.MAX_BODY_SIZE) {
                    clearTimeout(timer);
                    this._sendRaw(res, 413, "Payload Too Large");
                    req.destroy();
                    resolve(null);
                }
            });

            req.on("end", () => {
                clearTimeout(timer);
                resolve(data);
            });
        });
    }

    // --- FIX: Proper JSON-RPC Error Formatter ---
    _sendError(res, code, message, id) {
        this._send(res, { jsonrpc: "2.0", error: { code, message }, id });
    }

    _send(res, payload) {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify(payload));
    }

    _sendRaw(res, code, msg) {
        res.writeHead(code, { "Content-Type": "text/plain" });
        res.end(msg);
    }
}

module.exports = RPCServer;
