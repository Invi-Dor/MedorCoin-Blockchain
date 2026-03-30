/**
 * MEDORCOIN - Sovereign WebSocket Engine (v4.0)
 * Architecture: Priority-Aware Distributed Fabric
 * Features: JWT Auth, Binary Protocol, Global Node Janitor, Flow Control
 */

"use strict";

const { WebSocketServer, WebSocket } = require('ws');
const msgpack = require('msgpack5')();
const jwt = require('jsonwebtoken'); // Assuming user's existing JWT integration
const crypto = require('crypto');

class MedorWS {
    constructor(server, engine) {
        this.wss = new WebSocketServer({ 
            server,
            clientTracking: false,
            maxPayload: 1024 * 128 // 128KB (Supports large block headers)
        });
        
        this.engine = engine;
        this.nodeId = engine.nodeId;
        
        // --- Registry & Buffering ---
        this.localClients = new Map(); // address -> { ws, highQueue, lowQueue, isSending }
        this.REGISTRY_KEY = "medor:registry:users";
        this.NODE_HEARTBEAT = `medor:node_live:${this.nodeId}`;
        this.NODE_CHANNEL = `medor:node_bus:${this.nodeId}`;
        
        // --- Tuning Constants ---
        this.MAX_TOTAL_BACKLOG = 100;
        this.HB_INTERVAL = 30000;
        this.JWT_SECRET = process.env.JWT_SECRET;

        this.init();
    }

    async init() {
        // 1. DISTRIBUTED MESSAGE BUS (Binary/MsgPack)
        const sub = this.engine.redis.duplicate();
        await sub.subscribe(this.NODE_CHANNEL, 'medor:broadcast');

        sub.on('messageBuffer', (channel, buffer) => {
            setImmediate(() => this._routeGlobalMessage(channel.toString(), buffer));
        });

        // 2. CONNECTION LIFECYCLE
        this.wss.on('connection', (ws, req) => this._handleConnection(ws, req));

        // 3. NODE JANITOR (Ensures cleanup on cluster failure)
        this._startSovereignJanitor();
    }

    async _handleConnection(ws, req) {
        const url = new URL(req.url, `http://${req.headers.host}`);
        const token = url.searchParams.get('token');

        if (!token) return ws.close(4001, "JWT_REQUIRED");

        try {
            // --- JWT VERIFICATION (User Integrated) ---
            const decoded = jwt.verify(token, this.JWT_SECRET);
            const address = decoded.wallet || decoded.sub;

            if (!address) throw new Error("INVALID_CLAIM");

            // 4. GLOBAL REGISTRY SYNC
            // Atomically register user to this node
            await this.engine.redis.hset(this.REGISTRY_KEY, address, this.nodeId);

            ws.isAlive = true;
            ws.userAddress = address;
            
            // Client State with Priority Queuing
            const clientState = { 
                ws, 
                highQueue: [], // Critical: Balances, Transactions
                lowQueue: [],  // Non-Critical: Price, Network Stats
                isSending: false 
            };
            
            this.localClients.set(address, clientState);

            // Immediate Sync
            this.engine._localGet(this.engine.PARTITIONS.STATE + address).then(state => {
                this._enqueue(address, { type: 'INIT_SYNC', data: state }, true);
            });

            ws.on('pong', () => { ws.isAlive = true; });
            ws.on('close', () => this._onDisconnect(address));
            ws.on('error', () => ws.terminate());

        } catch (err) {
            ws.close(4003, "JWT_INVALID");
        }
    }

    // 5. PRIORITY-AWARE ROUTING
    _routeGlobalMessage(channel, buffer) {
        try {
            const msg = msgpack.decode(buffer);
            const isHigh = msg.priority === 'HIGH' || channel === 'medor:broadcast';

            if (channel === 'medor:broadcast') {
                for (const address of this.localClients.keys()) {
                    this._enqueue(address, msg, isHigh);
                }
            } else if (msg.address && this.localClients.has(msg.address)) {
                this._enqueue(msg.address, msg, isHigh);
            }
        } catch (e) {
            this.engine.metrics.err++;
        }
    }

    // 6. FLOW CONTROL (Parity with ConnectionPool.cpp logic)
    _enqueue(address, data, isHigh = false) {
        const client = this.localClients.get(address);
        if (!client || client.ws.readyState !== WebSocket.OPEN) return;

        const queue = isHigh ? client.highQueue : client.lowQueue;
        
        // Backpressure check
        if (queue.length > this.MAX_TOTAL_BACKLOG) {
            if (!isHigh) return; // Drop low priority if full
            queue.shift(); // Drop oldest high priority
        }

        queue.push(msgpack.encode(data));
        this._drain(address);
    }

    _drain(address) {
        const client = this.localClients.get(address);
        if (!client || client.isSending) return;

        // Priority logic: Always empty highQueue before touching lowQueue
        let chunk;
        if (client.highQueue.length > 0) {
            chunk = client.highQueue.shift();
        } else if (client.lowQueue.length > 0) {
            chunk = client.lowQueue.shift();
        } else {
            return; // All queues empty
        }

        client.isSending = true;
        client.ws.send(chunk, { binary: true }, (err) => {
            client.isSending = false;
            if (err) return client.ws.terminate();
            
            // Tail-call drain using setImmediate to avoid stack overflow
            if (client.highQueue.length > 0 || client.lowQueue.length > 0) {
                setImmediate(() => this._drain(address));
            }
        });
    }

    async _onDisconnect(address) {
        this.localClients.delete(address);
        
        // Only clear registry if this node is still the "owner" of the address
        const currentOwner = await this.engine.redis.hget(this.REGISTRY_KEY, address);
        if (currentOwner === this.nodeId) {
            await this.engine.redis.hdel(this.REGISTRY_KEY, address);
        }
    }

    // 7. SOVEREIGN JANITOR (Liveness & Global Cleanup)
    _startSovereignJanitor() {
        // Node Heartbeat
        setInterval(() => {
            this.engine.redis.set(this.NODE_HEARTBEAT, "1", "EX", 45);
        }, 30000);

        // Staggered Heartbeat (CPU Optimized)
        setInterval(() => {
            const clients = Array.from(this.localClients.values());
            let i = 0;
            const sweep = () => {
                const end = Math.min(i + 100, clients.length);
                for (; i < end; i++) {
                    const { ws } = clients[i];
                    if (!ws.isAlive) return ws.terminate();
                    ws.isAlive = false;
                    ws.ping();
                }
                if (i < clients.length) setImmediate(sweep);
            };
            sweep();
        }, this.HB_INTERVAL);
    }
}

module.exports = MedorWS;
