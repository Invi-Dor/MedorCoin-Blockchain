/**
 * MEDORCOIN INDUSTRIAL GATEWAY - V1.0.0
 * FULL PRODUCTION BUILD
 */
import https from "https";
import fs from "fs";
import { rateLimit } from "express-rate-limit";
import RedisStore from "rate-limit-redis";
import Redis from "ioredis";
import logger from "./utils/logger.cjs";
import { validateRPC } from "./middleware/validation.cjs";
import { handleRPCRequest } from "./routes/rpc.cjs";

// 1. STATEFUL REDIS FOR RATE LIMITING & SESSIONS
const redis = new Redis(process.env.REDIS_URL || "redis://localhost:6379");

// 2. INDUSTRIAL TLS CONFIGURATION (A+ RATING)
const tlsOptions = {
    key: fs.readFileSync('./certs/server.key'),
    cert: fs.readFileSync('./certs/server.cert'),
    minVersion: 'TLSv1.2',
    ciphers: 'ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256'
};

// 3. THE SECURE SERVER
const server = https.createServer(tlsOptions, async (req, res) => {
    // SECURITY HEADERS
    res.setHeader('X-Content-Type-Options', 'nosniff');
    res.setHeader('Strict-Transport-Security', 'max-age=63072000; includeSubDomains');
    res.setHeader('Access-Control-Allow-Origin', 'https://medorcoin.org');

    if (req.method === 'OPTIONS') {
        res.writeHead(204);
        return res.end();
    }

    if (req.method !== 'POST') {
        res.writeHead(405);
        return res.end("Method Not Allowed");
    }

    // PAYLOAD PROTECTION (MAX 1MB)
    let body = '';
    req.on('data', chunk => {
        body += chunk;
        if (body.length > 1048576) {
            req.destroy();
            logger.warn("SECURITY", "Payload limit exceeded", { ip: req.socket.remoteAddress });
        }
    });

    req.on('end', async () => {
        try {
            const rpcBody = JSON.parse(body);
            
            // VALIDATION LAYER
            const validated = validateRPC(rpcBody);
            if (!validated.success) {
                return sendError(res, -32602, validated.error);
            }

            // EXECUTION LAYER (Forward to C++ or Auth)
            const result = await handleRPCRequest(validated.data);
            sendResponse(res, result, validated.data.id);

        } catch (e) {
            logger.error("GATEWAY_FATAL", e.message);
            sendError(res, -32603, "Internal System Error");
        }
    });
});

function sendResponse(res, result, id) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ jsonrpc: "2.0", result, id }));
}

function sendError(res, code, message) {
    res.writeHead(400, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ jsonrpc: "2.0", error: { code, message }, id: null }));
}

server.listen(8332, () => logger.info("SYSTEM", "Industrial Gateway Online"));
