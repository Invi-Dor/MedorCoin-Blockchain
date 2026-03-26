/**
 * MedorCoin - P2P Network Module (Final Production Version)
 * * Integrated Solutions:
 * 1. Exponential Backoff: Prevents CPU/Network churn on failing peers.
 * 2. Consensus Integration: Handshake now exchanges actual local chain height.
 * 3. Heartbeat Tuning: Fully configurable stale multipliers and ping intervals.
 * 4. Resource Cleanup: Atomic shutdown of all pending timers and sockets.
 */

const net = require("net");
const EventEmitter = require("events");
const logger = require("./logger");
const crypto = require("crypto");

class Peer extends EventEmitter {
    constructor(socket, address, port, inbound = false) {
        super();
        this.socket = socket;
        this.address = address;
        this.port = port;
        this.id = `${address}:${port}`;
        this.inbound = inbound;
        
        this.connected = true;
        this.handshaked = false;
        this.lastSeen = Date.now();
        this.score = 100;
        this.version = null;
        this.height = 0;
        
        this.messagesSeen = new Map();
        this.buffer = "";
        this.hsTimer = null; 
    }

    send(type, payload) {
        if (!this.connected) return;
        try {
            const msg = JSON.stringify({ type, payload, ts: Date.now() });
            this.socket.write(msg + "\n");
        } catch (err) {
            logger.error("P2P", `[${this.id}] Send failed: ${err.message}`);
            this.destroy();
        }
    }

    isDuplicate(msgHash) {
        if (this.messagesSeen.has(msgHash)) return true;
        this.messagesSeen.set(msgHash, Date.now());
        if (this.messagesSeen.size > 10000) this._pruneCache();
        return false;
    }

    _pruneCache() {
        const entries = Array.from(this.messagesSeen.entries());
        entries.sort((a, b) => a[1] - b[1]);
        const toRemove = entries.slice(0, 2500);
        for (const [hash] of toRemove) this.messagesSeen.delete(hash);
    }

    destroy() {
        if (this.hsTimer) clearTimeout(this.hsTimer);
        this.connected = false;
        this.socket.destroy();
        this.emit("close");
    }
}

class P2PNetwork extends EventEmitter {
    constructor(chain, config = {}) { // Fix: Pass chain for height integration
        super();
        this.chain = chain;
        this.port = config.port || 8333;
        this.seedPeers = config.seedPeers || [];
        
        // Protocol & Heartbeat Tuning
        this.MAX_PEERS = config.maxPeers || 25;
        this.MIN_PEERS = config.minPeers || 4;
        this.PING_INTERVAL = config.pingInterval || 30000;
        this.STALE_MULTIPLIER = config.staleMultiplier || 3; 
        this.RECONNECT_BASE_DELAY = 5000;
        this.RECONNECT_MAX_DELAY = 60000;

        this.peers = new Map();
        this.bannedPeers = new Set();
        this.reconnectTimers = new Map(); 
        this.reconnectAttempts = new Map(); // Fix: Track attempts for backoff
        
        this.server = null;
        this.pingTimer = null;
    }

    start() {
        this._listen();
        this._connectToSeeds();
        this._startPingLoop();
        logger.info("P2P", `Network service online on port ${this.port}`);
    }

    _listen() {
        if (this.server) this.server.close();
        this.server = net.createServer((socket) => this._setupSocket(socket, true));
        this.server.on("error", (err) => {
            logger.error("P2P", `Listener Error: ${err.message}`);
            setTimeout(() => this._listen(), this.RECONNECT_BASE_DELAY);
        });
        this.server.listen(this.port);
    }

    connectTo(address, port) {
        const peerId = `${address}:${port}`;
        if (this.peers.has(peerId) || this.bannedPeers.has(address)) return;
        if (this.peers.size >= this.MAX_PEERS) return;

        const socket = net.connect(port, address, () => {
            this._setupSocket(socket, false, address, port);
            this.reconnectAttempts.delete(peerId); // Reset backoff on success
        });

        socket.on("error", (err) => {
            logger.warn("P2P", `[${peerId}] Connection failed: ${err.message}`);
            this._scheduleReconnect(address, port);
        });
    }

    _setupSocket(socket, inbound, addr, prt) {
        const address = addr || socket.remoteAddress;
        const port = prt || socket.remotePort;
        const peer = new Peer(socket, address, port, inbound);

        if (this.bannedPeers.has(address) || this.peers.size >= this.MAX_PEERS) {
            socket.destroy();
            return;
        }

        peer.hsTimer = setTimeout(() => {
            if (!peer.handshaked) {
                logger.warn("P2P", `[${peer.id}] Handshake timed out.`);
                peer.destroy();
            }
        }, 10000);

        socket.on("data", (data) => {
            peer.buffer += data.toString();
            const lines = peer.buffer.split("\n");
            peer.buffer = lines.pop();
            for (const line of lines) {
                if (line.trim()) this._handleMessage(peer, line);
            }
        });

        socket.on("close", () => this._handleDisconnect(peer));
        socket.on("error", () => peer.destroy());

        this.peers.set(peer.id, peer);
        
        // Fix: Send actual chain height in handshake
        const currentHeight = this.chain ? this.chain.getLatestBlock().height : 0;
        peer.send("version", { version: "1.0.0", height: currentHeight });
    }

    _handleMessage(peer, raw) {
        try {
            const msg = JSON.parse(raw);
            peer.lastSeen = Date.now();

            if (msg.type === "version") {
                if (peer.hsTimer) clearTimeout(peer.hsTimer);
                peer.handshaked = true;
                peer.version = msg.payload.version;
                peer.height = msg.payload.height;
                logger.info("P2P", `[${peer.id}] Handshaked (Peer Height: ${peer.height})`);
                this.emit("peer:connect", peer);
                return;
            }

            if (!peer.handshaked) return;

            const msgHash = crypto.createHash('sha256').update(raw).digest('hex');
            if (peer.isDuplicate(msgHash)) return;

            if (msg.type === "ping") return peer.send("pong", {});
            
            this.emit("message", msg, peer.id);
        } catch (err) {
            this.penalizePeer(peer.id, 20, "INVALID_PROTOCOL_MSG");
        }
    }

    penalizePeer(peerId, amount, reason) {
        const peer = this.peers.get(peerId);
        if (!peer) return;

        peer.score -= amount;
        logger.warn("P2P", `[${peer.id}] Penalized: ${reason}. Score: ${peer.score}`);

        if (peer.score <= 0) {
            this.bannedPeers.add(peer.address);
            peer.destroy();
        }
    }

    _handleDisconnect(peer) {
        this.peers.delete(peer.id);
        if (!this.bannedPeers.has(peer.address) && this.peers.size < this.MIN_PEERS) {
            this._scheduleReconnect(peer.address, peer.port);
        }
    }

    _scheduleReconnect(address, port) {
        const id = `${address}:${port}`;
        if (this.reconnectTimers.has(id)) return;

        // Fix: Exponential Backoff Strategy
        const attempts = (this.reconnectAttempts.get(id) || 0) + 1;
        this.reconnectAttempts.set(id, attempts);
        
        const delay = Math.min(
            this.RECONNECT_BASE_DELAY * Math.pow(2, attempts - 1), 
            this.RECONNECT_MAX_DELAY
        );

        logger.info("P2P", `[${id}] Scheduling retry #${attempts} in ${delay}ms`);
        
        const timer = setTimeout(() => {
            this.reconnectTimers.delete(id);
            this.connectTo(address, port);
        }, delay);

        this.reconnectTimers.set(id, timer);
    }

    broadcast(type, payload, excludePeerId = null) {
        for (const [id, peer] of this.peers) {
            if (id === excludePeerId || !peer.handshaked) continue;
            peer.send(type, payload);
        }
    }

    _connectToSeeds() {
        this.seedPeers.forEach(p => this.connectTo(p.address, p.port));
    }

    _startPingLoop() {
        this.pingTimer = setInterval(() => {
            const now = Date.now();
            const staleThreshold = this.PING_INTERVAL * this.STALE_MULTIPLIER;
            for (const [id, peer] of this.peers) {
                if (now - peer.lastSeen > staleThreshold) {
                    logger.warn("P2P", `[${id}] Stale timeout.`);
                    peer.destroy();
                } else {
                    peer.send("ping", {});
                }
            }
        }, this.PING_INTERVAL);
    }

    stop() {
        clearInterval(this.pingTimer);
        for (const timer of this.reconnectTimers.values()) clearTimeout(timer);
        this.reconnectTimers.clear();
        this.peers.forEach(p => p.destroy());
        this.peers.clear();
        if (this.server) this.server.close();
        logger.info("P2P", "P2P System terminated.");
    }
}

module.exports = P2PNetwork;
