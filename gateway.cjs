import 'dotenv/config';
import https from "https";
import fs from "fs";
import path from "path";
import { rateLimit } from "express-rate-limit"; 
import RedisStore from "rate-limit-redis"; 
import Redis from "ioredis";
import jwt from "jsonwebtoken";
import { z } from "zod"; // FIX 1: Schema Validation
import { handleRPCRequest } from "./routes/rpc.cjs";
import logger from "./logger.js";
import Mempool from "./mempool.cjs";
import Metrics from "./metrics.js";

const redis = new Redis(process.env.REDIS_URL || "redis://localhost:6379");
const metrics = new Metrics();
metrics.useRedis(redis);
const mempool = new Mempool(); 

// FIX 1: Strict JSON-RPC Schema
const RPCSchema = z.object({
    jsonrpc: z.literal("2.0"),
    method: z.string().min(1),
    params: z.array(z.any()).optional(),
    id: z.union([z.string(), z.number()]).nullable()
});

const limiter = rateLimit({
    windowMs: 15 * 60 * 1000, 
    max: 100, 
    standardHeaders: true,
    store: new RedisStore({ sendCommand: (...args) => redis.call(...args) }),
});

const tlsOptions = {
    key: fs.readFileSync(path.resolve('./certs/server.key')),
    cert: fs.readFileSync(path.resolve('./certs/server.cert')),
    minVersion: 'TLSv1.2',
    ciphers: 'ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256', // FIX 5: Hardened Ciphers
    honorCipherOrder: true
};

const server = https.createServer(tlsOptions, (req, res) => {
    const start = performance.now();

    // FIX 5: Hardened Security Headers
    res.setHeader('Access-Control-Allow-Origin', 'https://medorcoin.org');
    res.setHeader('X-Content-Type-Options', 'nosniff');
    res.setHeader('X-Frame-Options', 'DENY');
    res.setHeader('Strict-Transport-Security', 'max-age=63072000; includeSubDomains; preload');
    res.setHeader('Content-Security-Policy', "default-src 'none'; frame-ancestors 'none';");

    limiter(req, res, async () => {
        if (req.method === 'GET' && req.url === '/api/stats') {
            try {
                // FIX 7: Fallback for C++ failure
                const stats = await mempool.getMiningTemplate() || { blocks: 0, hashrate: "0" };
                res.writeHead(200, { 'Content-Type': 'application/json' });
                return res.end(JSON.stringify({ height: stats.blocks, hashrate: stats.hashrate }));
            } catch (e) {
                return sendJSONRPCError(res, -32603, "C++ Engine Offline", null);
            }
        }

        let body = '';
        req.on('data', chunk => { body += chunk; if (body.length > 524288) req.destroy(); });
        req.on('end', async () => {
            let rpcReq;
            try {
                rpcReq = JSON.parse(body);
                
                // FIX 1: Schema Validation
                const validated = RPCSchema.parse(rpcReq);

                if (validated.method !== 'medor_login') {
                    const authHeader = req.headers.authorization;
                    if (!authHeader) throw { code: -32001, message: "UNAUTHORIZED: MISSING_TOKEN" };
                    
                    const token = authHeader.split(' ')[1];
                    const decoded = jwt.verify(token, process.env.JWT_SECRET);
                    
                    if (await redis.get(`blacklist:${decoded.sub}`)) throw { code: -32003, message: "SESSION_REVOKED" };
                }

                // FIX 3: Pass to Router with Atomic context
                const result = await handleRPCRequest(validated, mempool);
                
                res.writeHead(200, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ jsonrpc: "2.0", result, id: validated.id }));

            } catch (err) {
                // FIX 2: Standardized JSON-RPC Error Responses
                const code = err.code || (err instanceof z.ZodError ? -32602 : -32603);
                const msg = err.message || "Internal Error";
                sendJSONRPCError(res, code, msg, rpcReq?.id || null);
            } finally {
                metrics.observe("rpc_latency_ms", performance.now() - start);
            }
        });
    });
});

function sendJSONRPCError(res, code, message, id) {
    logger.warn("GATEWAY_ERR", `Code: ${code} | Msg: ${message}`);
    res.writeHead(code === -32001 ? 401 : 400, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ jsonrpc: "2.0", error: { code, message }, id }));
}

// FIX 4: Process Supervision - Use PM2 to manage this file
server.listen(8332, () => logger.info("SYSTEM", "MedorCoin Industrial Gateway Locked & Online (8332)"));
