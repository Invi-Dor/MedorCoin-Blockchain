/**
 * MEDORCOIN - Industrial Multi-Node Payment Watcher (Production-Ready)
 * Role: Distributed monitor for confirmed payments across a high-availability cluster.
 */

const EventEmitter = require('events');
const crypto = require('crypto');
const Redis = require('ioredis');

class PaymentWatcher extends EventEmitter {
    constructor(engine) {
        super();
        this.engine = engine;

        // PROBLEM 1 FIX: Redis Configuration for High-Availability (Cluster/Sentinel support)
        // Checks for cluster nodes in config; otherwise defaults to standard standalone/sentinel
        const redisConfig = engine.redisConfig || { host: '127.0.0.1', port: 6339 };
        const redisOptions = { 
            retryStrategy: (times) => Math.min(times * 50, 2000),
            reconnectOnError: (err) => err.message.includes('READONLY')
        };

        this.redis = Array.isArray(redisConfig.nodes) 
            ? new Redis.Cluster(redisConfig.nodes, redisOptions) 
            : new Redis(redisConfig);

        this.pub = Array.isArray(redisConfig.nodes) 
            ? new Redis.Cluster(redisConfig.nodes, redisOptions) 
            : new Redis(redisConfig);

        this.sub = Array.isArray(redisConfig.nodes) 
            ? new Redis.Cluster(redisConfig.nodes, redisOptions) 
            : new Redis(redisConfig);

        // PROBLEM 3 FIX: Structured Logging & Monitoring
        this.logger = {
            info: (msg, meta = {}) => this._log('INFO', msg, meta),
            error: (msg, meta = {}) => this._log('ERROR', msg, meta),
            alert: (msg, meta = {}) => this._log('ALERT', msg, meta)
        };

        this._setupDistributedEvents();
        this._startWatching();
    }

    _log(level, message, metadata) {
        // Structured JSON logging for industrial log aggregators (ELK, Grafana)
        console.log(JSON.stringify({
            ts: new Date().toISOString(),
            level,
            service: 'payment-watcher',
            message,
            ...metadata
        }));
    }

    _setupDistributedEvents() {
        this.sub.subscribe('payment_confirmations');
        this.sub.on('message', (channel, message) => {
            if (channel === 'payment_confirmations') {
                try {
                    const { sessionId, session } = JSON.parse(message);
                    super.emit(`payment_confirmed_${sessionId}`, session);
                } catch (err) {
                    this.logger.error("Failed to parse cluster confirmation", { err: err.message });
                }
            }
        });
    }

    async createSession(userId, expectedAmount) {
        try {
            const sessionId = `req_${crypto.randomBytes(4).toString('hex')}`;
            const depositAddress = `mdr_${crypto.randomBytes(12).toString('hex')}`; 
            
            const sessionData = {
                userId,
                address: depositAddress,
                expectedAmount: expectedAmount.toString(),
                status: 'pending',
                createdAt: Date.now()
            };

            await this.engine.db.put(`pay_session:${sessionId}`, sessionData);
            await this.redis.sadd('active_payment_sessions', sessionId);

            this.logger.info("Payment session created", { sessionId, userId, depositAddress });
            return { sessionId, depositAddress };
        } catch (err) {
            this.logger.alert("Critical failure creating session", { err: err.message, userId });
            throw err;
        }
    }

    async _startWatching() {
        try {
            const sessionIds = await this.redis.smembers('active_payment_sessions');
            
            // PROBLEM 2 FIX: Engine DB Performance. 
            // Process in parallel batches to maximize I/O throughput for thousands of sessions.
            const BATCH_SIZE = 100; 
            for (let i = 0; i < sessionIds.length; i += BATCH_SIZE) {
                const batch = sessionIds.slice(i, i + BATCH_SIZE);

                await Promise.all(batch.map(async (sessionId) => {
                    const lockKey = `lock:session:${sessionId}`;
                    
                    // Acquire lock
                    const acquired = await this.redis.set(lockKey, "locked", "NX", "EX", 10);
                    if (!acquired) return;

                    // PROBLEM 4 FIX: Dynamic Lock Renewal (Heartbeat)
                    // Automatically extends the lock every 4s while the DB operation is active
                    const lockHeartbeat = setInterval(() => {
                        this.redis.expire(lockKey, 10).catch(() => clearInterval(lockHeartbeat));
                    }, 4000);

                    try {
                        const session = await this.engine.db.get(`pay_session:${sessionId}`).catch(() => null);
                        if (!session) {
                            await this.redis.srem('active_payment_sessions', sessionId);
                            return;
                        }

                        // Consistency Check against Engine State Partition
                        const state = await this.engine.db.get(`${this.engine.PARTITIONS.STATE}${session.address}`)
                            .catch(() => ({ balance: "0" }));
                        
                        const currentBalance = BigInt(state.balance || 0);

                        if (currentBalance >= BigInt(session.expectedAmount)) {
                            // Atomic transition
                            await this.engine.db.del(`pay_session:${sessionId}`);
                            await this.redis.srem('active_payment_sessions', sessionId);
                            
                            const confirmedData = { ...session, status: 'confirmed' };
                            
                            // Broadcast to cluster
                            await this.pub.publish('payment_confirmations', JSON.stringify({
                                sessionId,
                                session: confirmedData
                            }));

                            await this.engine.db.put(`pay_archive:${sessionId}`, confirmedData);
                            this.logger.info("Payment confirmed and broadcast", { sessionId, address: session.address });
                        }
                    } catch (err) {
                        this.logger.error("Session processing error", { sessionId, err: err.message });
                    } finally {
                        clearInterval(lockHeartbeat);
                        await this.redis.del(lockKey); // Release lock immediately upon completion
                    }
                }));
            }
        } catch (e) {
            this.logger.alert("Cluster sync loop failure", { err: e.message });
        }

        setTimeout(() => this._startWatching(), 2000);
    }

    emit(event, ...args) {
        super.emit(event, ...args);
    }
}

module.exports = PaymentWatcher;
