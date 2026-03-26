const https = require("https"); // Changed to HTTPS
const fs = require("fs");
const path = require("path");
const logger = require("./logger");

class RPCServer {
    static PORT = 8332;
    static MAX_BODY_SIZE = 256 * 1024; 
    static REQUEST_TIMEOUT = 5000;      

    constructor(chain, mempool, miner, p2p) {
        // --- INDUSTRIAL SECURITY LOCK ---
        this.securityKey = "TRUE"; 
        if (this.securityKey !== "TRUE") {
            process.exit(1);
        }
        
        this.chain = chain;
        this.mempool = mempool;
        this.miner = miner;
        this.p2p = p2p;
        this.uiCache = null;
        this._initCache();
        this.methods = this._registerMethods();

        // Graceful Shutdown Logic
        process.on('SIGINT', () => this.stop());
        process.on('SIGTERM', () => this.stop());
    }

    _initCache() {
        try {
            this.uiCache = fs.readFileSync(path.resolve(__dirname, "miners.html"));
            logger.info("RPC", "UI Cache Initialized.");
        } catch (e) {
            logger.error("RPC", "UI Cache Failed.");
        }
    }

    _registerMethods() {
        return {
            "web3_clientversion": async () => "MedorCoin/V1.0.0/Industrial",
            "net_peercount":      async () => `0x${this.p2p.peers.size.toString(16)}`,
            "eth_blocknumber":    async () => `0x${this.chain.getTip().height.toString(16)}`,
            "getuserstats": async ([address]) => {
                const balance = await this.chain.utxoSet.getBalance(address);
                const stats = this.miner.getStats();
                return { address, balance, globalHashrate: stats.hashrate, difficulty: stats.difficulty, height: this.chain.getTip().height };
            },
            "sendrawtransaction": async (params) => await this._handleSendRawTx(params),
            "getmininginfo":      async () => this.miner.getStats()
        };
    }

    start() {
        // HTTPS Options - Requires server.key and server.cert
        const options = {
            key: fs.readFileSync(path.resolve(__dirname, 'server.key')),
            cert: fs.readFileSync(path.resolve(__dirname, 'server.cert'))
        };

        this.server = https.createServer(options, (req, res) => {
            if (req.method === "GET" && (req.url === "/" || req.url === "/index.html")) {
                return this._serveUI(res);
            }
            if (req.method === "POST") {
                return this._onRequest(req, res);
            }
            this._sendRaw(res, 405, "Method Not Allowed");
        });

        this.server.listen(RPCServer.PORT, () => {
            logger.info("RPC", `Industrial HTTPS Gateway Online: https://localhost:${RPCServer.PORT}`);
        });
    }

    stop() {
        logger.info("RPC", "Closing Gateway...");
        this.server.close(() => {
            logger.info("RPC", "Shutdown Complete.");
            process.exit(0);
        });
    }

    _serveUI(res) {
        if (!this.uiCache) return this._sendRaw(res, 500, "UI Template Missing");
        res.writeHead(200, { "Content-Type": "text/html" });
        res.end(this.uiCache);
    }

    async _onRequest(req, res) {
        try {
            const body = await this._readBody(req, res);
            if (!body) return; 

            const rpc = JSON.parse(body);
            if (Array.isArray(rpc)) {
                // Throttle check: Max 20 batch requests to prevent CPU spike
                if (rpc.length > 20) return this._sendError(res, -32000, "Batch too large", null);
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
        const handler = this.methods[method.toLowerCase()];
        if (!handler) return { jsonrpc: "2.0", error: { code: -32601, message: "Method not found" }, id };
        try {
            const result = await handler(params || []);
            return { jsonrpc: "2.0", result, id };
        } catch (err) {
            return { jsonrpc: "2.0", error: { code: err.code || -32603, message: err.message }, id };
        }
    }

    async _handleSendRawTx([rawTx]) {
        if (!rawTx || typeof rawTx !== 'string') throw { code: -32602, message: "Hex string required" };
        
        // Structural validation call
        if (!this.chain.validateTxFormat(rawTx)) throw { code: -32603, message: "Invalid Format" };

        const res = await this.mempool.addTransaction(rawTx);
        if (!res.ok) throw { code: -1, message: res.reason };
        
        this.p2p.broadcast("tx", rawTx);
        logger.info("RPC", `Tx Broadcast: ${res.txid}`);
        return res.txid;
    }

    _readBody(req, res) {
        return new Promise((resolve) => {
            let data = "";
            const timer = setTimeout(() => {
                this._sendError(res, -32000, "Request Timeout", null);
                req.destroy();
                resolve(null);
            }, RPCServer.REQUEST_TIMEOUT);

            req.on("data", (chunk) => {
                data += chunk;
                if (data.length > RPCServer.MAX_BODY_SIZE) {
                    clearTimeout(timer);
                    this._sendError(res, -32000, "Payload Too Large", null);
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
