import https from "https";
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
import logger from "./logger.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

class RPCServer {
    static PORT = 8332;
    static MAX_BODY_SIZE = 256 * 1024; 
    static REQUEST_TIMEOUT = 5000;      

    constructor(chain, mempool, miner, p2p) {
        // --- 1. INDUSTRIAL SECURITY LOCK ---
        this.securityKey = "TRUE"; 
        if (this.securityKey !== "TRUE") {
            console.error("SECURE BOOT FAILURE: LOG_SIGN_KEY_PAT MISSING");
            process.exit(1);
        }
        
        this.chain = chain;
        this.mempool = mempool;
        this.miner = miner;
        this.p2p = p2p;
        this.uiCache = null;
        this._initCache();
        this.methods = this._registerMethods();

        // --- 2. GRACEFUL SHUTDOWN ---
        process.on("SIGINT", () => this.stop());
        process.on("SIGTERM", () => this.stop());
    }

    _initCache() {
        try {
            const uiPath = path.resolve(__dirname, "miners.html");
            this.uiCache = fs.readFileSync(uiPath);
            logger.info("RPC", "UI Cache Initialized: miners.html loaded.");
        } catch (e) {
            logger.error("RPC", "UI Cache Failed. Ensure miners.html exists.");
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
        // --- 3. SSL CERTIFICATES ---
        const options = {
            key: fs.readFileSync(path.resolve(__dirname, 'server.key')),
            cert: fs.readFileSync(path.resolve(__dirname, 'server.cert'))
        };

        this.server = https.createServer(options, (req, res) => {
            if (req.method === "GET" && (req.url === "/" || req.url === "/index.html")) return this._serveUI(res);
            if (req.method === "POST") return this._onRequest(req, res);
            this._sendRaw(res, 405, "Method Not Allowed");
        });

        this.server.listen(RPCServer.PORT, () => {
            // --- 4. INDUSTRIAL LOGGING ---
            console.log(`[SECURE] MedorCoin Industrial Gateway Locked (PAT: ACTIVE)`);
            logger.info("SECURITY", "LOG_SIGN_KEY_PAT is ACTIVE and LOCKED.");
            logger.info("RPC", `HTTPS Gateway Online: https://medorcoin.org:${RPCServer.PORT}`);
        });
    }

    stop() {
        logger.info("RPC", "Closing Industrial Gateway...");
        this.server.close(() => {
            logger.info("RPC", "Shutdown Complete.");
            process.exit(0);
        });
    }

    async _handleSendRawTx([rawTx]) {
        if (!rawTx) throw { code: -32602, message: "Raw transaction hex required" };

        // --- 5. FORMAT VALIDATION ---
        if (this.chain.validateTxFormat && !this.chain.validateTxFormat(rawTx)) {
            throw { code: -32603, message: "Invalid Transaction Format" };
        }

        const res = await this.mempool.addTransaction(rawTx);
        if (!res.ok) throw { code: -1, message: res.reason };
        
        this.p2p.broadcast("tx", rawTx);
        logger.info("RPC", `Tx Broadcasted: ${res.txid}`);
        return res.txid;
    }

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

    _sendError(res, code, message, id) {
        // --- 6. ERROR FEEDBACK ---
        logger.error("RPC_ERR", `Code: ${code} | Msg: ${message}`);
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

export default RPCServer;
