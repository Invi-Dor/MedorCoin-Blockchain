/**
 * MedorCoin - Production Transport Module (log_transport.cjs)
 * Solves: Persistence, Backoff, Priority Batching, Encryption, Multi-Protocol, Structured Formatting.
 */

const https = require("https");
const http = require("http");
const dgram = require("dgram");
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const config = require("./log_config.cjs");

// Transport Configuration
const TRANSPORT_URL = config.transport.url || null;
const TRANSPORT_APIKEY = config.transport.apiKey || null;
const SYSLOG_HOST = config.transport.syslogHost || null;
const SYSLOG_PORT = config.transport.syslogPort || 514;
const TRANSPORT_TIMEOUT = config.transport.timeoutMs || 3000;
const ENCRYPTION_KEY = config.transport.encryptionKey; // Must be 32 bytes for AES-256

// Scaling & Queue Configuration
const BATCH_SIZE = config.transport.batchSize || 100;
const MAX_QUEUE_SIZE = 50000; // Drops normal logs if breached under heavy load
const WAL_PATH = path.join(__dirname, "logs_pending_wal.dat");

// Priority Queues (Memory)
let _queues = {
    critical: [],
    normal: []
};

let _isFlushing = false;
let _flushTimer = null;

// 1. PERSISTENCE: Load Write-Ahead Log (WAL) on startup
function _loadWAL() {
    if (fs.existsSync(WAL_PATH)) {
        try {
            const lines = fs.readFileSync(WAL_PATH, "utf8").split("\n").filter(Boolean);
            lines.forEach(line => {
                const parsed = JSON.parse(line);
                if (parsed.level === "CRITICAL" || parsed.level === "ERROR") {
                    _queues.critical.push(parsed);
                } else {
                    _queues.normal.push(parsed);
                }
            });
            fs.unlinkSync(WAL_PATH); // Clear WAL after loading into memory
        } catch (err) {
            process.stderr.write(`[LOG_TRANSPORT] WAL corrupted. Starting fresh.\n`);
        }
    }
}
_loadWAL();

if (TRANSPORT_URL || SYSLOG_HOST) {
    _flushTimer = setInterval(_flushBatch, config.transport.flushIntervalMs || 5000);
    if (_flushTimer.unref) _flushTimer.unref();
}

// 2. STRUCTURED LOGGING: Enforce JSON format before queuing
async function shipToTransport(level, subsystem, message, meta = {}) {
    if (!TRANSPORT_URL && !SYSLOG_HOST) return;

    const logEntry = {
        ts: new Date().toISOString(),
        level: level.toUpperCase(),
        subsystem: subsystem,
        msg: message,
        txId: meta.txId || null,
        userId: meta.userId || null,
        node: config.nodeId || "medorcoin-node"
    };

    const isCritical = (logEntry.level === "CRITICAL" || logEntry.level === "ERROR");

    // 3. SCALABILITY: Backpressure management
    if (_queues.normal.length + _queues.critical.length > MAX_QUEUE_SIZE) {
        if (!isCritical) return; // Drop normal logs to save memory for criticals
        _queues.critical.shift(); // Drop oldest critical if absolute max reached
    }

    const targetQueue = isCritical ? _queues.critical : _queues.normal;
    targetQueue.push(logEntry);

    // Write to WAL synchronously to survive node crashes
    fs.appendFileSync(WAL_PATH, JSON.stringify(logEntry) + "\n");

    if (targetQueue.length >= BATCH_SIZE) {
        await _flushBatch();
    }
}

// 4. PRIORITY BATCHING: Flush criticals first
async function _flushBatch(retryCount = 0, batchData = null) {
    if (_isFlushing && !batchData) return;
    if (_queues.critical.length === 0 && _queues.normal.length === 0 && !batchData) return;

    _isFlushing = true;

    // Pull from critical first, then fill remainder with normal
    let batch = batchData;
    if (!batch) {
        batch = _queues.critical.splice(0, BATCH_SIZE);
        if (batch.length < BATCH_SIZE) {
            batch = batch.concat(_queues.normal.splice(0, BATCH_SIZE - batch.length));
        }
    }

    if (batch.length === 0) {
        _isFlushing = false;
        return;
    }

    const payloadObj = {
        node: config.nodeId || "medorcoin-node",
        ts: new Date().toISOString(),
        lines: batch,
    };

    let finalPayload = JSON.stringify(payloadObj);

    // 5. ENCRYPTION: AES-256-GCM Payload Protection
    if (ENCRYPTION_KEY) {
        try {
            const iv = crypto.randomBytes(12);
            const cipher = crypto.createCipheriv("aes-256-gcm", Buffer.from(ENCRYPTION_KEY, 'hex'), iv);
            let encrypted = cipher.update(finalPayload, 'utf8', 'hex');
            encrypted += cipher.final('hex');
            const authTag = cipher.getAuthTag().toString('hex');
            
            finalPayload = JSON.stringify({
                encrypted: true,
                iv: iv.toString('hex'),
                authTag: authTag,
                data: encrypted
            });
        } catch (e) {
            process.stderr.write(`[LOG_TRANSPORT] Encryption failed: ${e.message}\n`);
            _isFlushing = false;
            return;
        }
    }

    try {
        // 6. MULTI-PROTOCOL: Ship to HTTP/S and/or Syslog
        if (TRANSPORT_URL) await _postHTTP(finalPayload);
        if (SYSLOG_HOST) await _postUDP(finalPayload);

        // Success: Clear the WAL file to reflect processed logs
        fs.writeFileSync(WAL_PATH, ""); 
        _isFlushing = false;

    } catch (err) {
        // 7. RETRY / EXPONENTIAL BACKOFF
        const maxRetries = 5;
        if (retryCount < maxRetries) {
            const backoffMs = Math.pow(2, retryCount) * 1000;
            process.stderr.write(`[LOG_TRANSPORT] Ship failed: ${err.message}. Retrying in ${backoffMs}ms (Attempt ${retryCount + 1}/${maxRetries})\n`);
            
            setTimeout(() => {
                _flushBatch(retryCount + 1, batch);
            }, backoffMs);
        } else {
            process.stderr.write(`[LOG_TRANSPORT] Max retries reached. Lines dropped: ${batch.length}\n`);
            _isFlushing = false;
        }
    }
}

function _postHTTP(payload) {
    return new Promise((resolve, reject) => {
        const url = new URL(TRANSPORT_URL);
        const driver = url.protocol === "https:" ? https : http;
        const options = {
            hostname: url.hostname,
            port: url.port || (url.protocol === "https:" ? 443 : 80),
            path: url.pathname,
            method: "POST",
            headers: {
                "Content-Type": "application/json",
                "Content-Length": Buffer.byteLength(payload),
                ...(TRANSPORT_APIKEY ? { "Authorization": `Bearer ${TRANSPORT_APIKEY}` } : {}),
            },
            timeout: TRANSPORT_TIMEOUT,
        };

        const req = driver.request(options, (res) => {
            res.resume();
            if (res.statusCode >= 400) reject(new Error(`HTTP ${res.statusCode}`));
            else resolve();
        });

        req.on("timeout", () => { req.destroy(); reject(new Error("Timeout")); });
        req.on("error", reject);
        req.write(payload);
        req.end();
    });
}

function _postUDP(payload) {
    return new Promise((resolve, reject) => {
        const client = dgram.createSocket('udp4');
        const message = Buffer.from(payload);
        client.send(message, 0, message.length, SYSLOG_PORT, SYSLOG_HOST, (err) => {
            client.close();
            if (err) reject(err);
            else resolve();
        });
    });
}

module.exports = { shipToTransport };
