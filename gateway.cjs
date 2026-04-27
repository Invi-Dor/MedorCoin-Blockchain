/**
 * FILE: medorcoin-node/gateway.cjs
 * UPDATED INDUSTRIAL BUILD
 * Integrated: Metrics, JWT Context, Zod Validation, and C++ Fallbacks.
 */

import 'dotenv/config';
import https from "https";
import fs from "fs";
import path from "path";
import { rateLimit } from "express-rate-limit"; 
import RedisStore from "rate-limit-redis"; 
import Redis from "ioredis";
import jwt from "jsonwebtoken";
import { z } from "zod";

// Local Modules
import { handleRPCRequest } from "./routes/rpc.cjs";
import logger from "./utils/logger.cjs";
import Mempool from "./mempool.cjs";
import Metrics from "./metrics.js"; // IMPORT METRICS

// 1. STATEFUL INITIALIZATION
const redis = new Redis(process.env.REDIS_URL || "redis://localhost:6379");
const metrics = new Metrics();
metrics.useRedis(redis); // Link Metrics to Redis for persistence
global.mempool = new Mempool(); 

// 2. RPC SCHEMA VALIDATION
const RPCSchema = z.object({
    jsonrpc: z.literal("2.0"),
    method: z.string().min(1),
    params: z.array(z.any()).optional(),
    id: z.union([z.string(), z.number()]).nullable()
});

// 3. PRODUCTION RATE LIMITER
const limiter = rateLimit({
    windowMs: 15 * 60 * 1000, 
    max: 100, 
    standardHeaders: true,
    store: new RedisStore({ sendCommand: (...args) => redis.call(...args) }),
});

// 4. TLS CONFIGURATION
const tlsOptions = {
    key: fs.readFileSync(path.resolve('./certs/server.key')),
    cert: fs.readFileSync(path.resolve('./certs/server.cert')),
    minVersion: 'TLSv1.2',
    ciphers: 'ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256',
    honorCipherOrder: true
};

const server = https.createServer(tlsOptions, (req, res) => {
    const start = performance.now();

    // SECURITY HEADERS
    res.setHeader('X-Content-Type-Options', 'nosniff');
    res.setHeader('X-Frame-Options', 'DENY');
    res.setHeader('Strict-Transport-Security', 'max-age=63072000; includeSubDomains');
    res.setHeader('Access-Control-Allow-Origin', 'https://medorcoin.org');
    res.setHeader('Content-Security-Policy', "default-src 'none'; frame-ancestors 'none';");

    limiter(req, res, async () => {
        const { method, url } = req;

        // A. PUBLIC API PROXY
        if (method === 'GET' && url === '/api/stats') {
            return servePublicStats(res);
        }

        // B. SECURE RPC HANDLER
        if (method === 'POST') {
            return processSecureRPC(req, res, start);
        }

        res.writeHead(404);
        res.end(JSON.stringify({ error: "NOT_FOUND" }));
    });
});

async function servePublicStats(res) {
    try {
        const stats = await global.mempool.getMiningTemplate() || { blocks: 0, hashrate: "0" }; 
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ height: stats.blocks, hashrate: stats.hashrate }));
    } catch (err) {
        res.writeHead(503);
        res.end(JSON.stringify({ error: "Node Busy" }));
    }
}

async function processSecureRPC(req, res, startTime) {
    let body = '';
    req.on('data', chunk => {
        body += chunk;
        if (body.length > 524288) req.destroy();
    });

    req.on('end', async () => {
        let rpcReq;
        try {
            rpcReq = JSON.parse(body);
            
            // 1. SCHEMA VALIDATION
            const validated = RPCSchema.parse(rpcReq);

            // 2. JWT & BLACKLIST CONTEXT
            let user = null;
            if (validated.method !== 'medor_login') {
                const authHeader = req.headers.authorization;
                if (!authHeader) throw { code: -32001, message: "UNAUTHORIZED: TOKEN_MISSING" };
                
                const token = authHeader.split(' ')[1];
                user = jwt.verify(token, process.env.JWT_SECRET);
                
                const isBlacklisted = await redis.get(`blacklist:${user.sub}`);
                if (isBlacklisted) throw { code: -32003, message: "SESSION_REVOKED" };
            }

            // 3. EXECUTE ROUTER (Passes all required context)
            const result = await handleRPCRequest(
                { ...validated, user }, 
                global.mempool, 
                redis, 
                metrics
            );
            
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ jsonrpc: "2.0", result, id: validated.id }));

        } catch (err) {
            const code = err.code || -32603;
            const msg = err.message || "Internal Error";
            logger.warn("GATEWAY_SEC", `Rejection: ${msg}`);
            
            res.writeHead(code === -32001 ? 401 : 400, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ 
                jsonrpc: "2.0", 
                error: { code, message: msg }, 
                id: rpcReq?.id || null 
            }));
        } finally {
            // 4. METRICS RECORDING
            metrics.observe("mdc_rpc_latency_ms", performance.now() - startTime, { method: rpcReq?.method || "unknown" });
        }
    });
}

server.listen(process.env.RPC_PORT || 8332, () => {
    logger.info("SYSTEM", "MedorCoin Industrial Gateway Locked & Online");
});
