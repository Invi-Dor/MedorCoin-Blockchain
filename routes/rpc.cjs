/**
 * FILE: medorcoin-node/routes/rpc.cjs
 * FINAL INDUSTRIAL BUILD: Atomic RPC Switchboard
 * Integrates: RBAC, Zod Validation, Distributed Locking, and Metrics.
 */
import { verifyUser } from "../middleware/auth.cjs";
import { findUserInDB } from "../services/db_service.cjs";
import logger from "../logger.js";
import { z } from "zod";

// --- 1. STRICT SCHEMAS (Validation & Sanitization) ---
const BridgeSchema = z.tuple([
    z.number().positive(),                 // Amount (Numeric)
    z.enum(["BTC", "ETH", "TUSD"]),        // Target Asset
    z.string().regex(/^[a-zA-Z0-9]{26,62}$/) // Destination Address
]);

const TxSchema = z.string().min(64).regex(/^[a-fA-F0-9]+$/);

// --- 2. THE HARDENED RPC ROUTER ---
export async function handleRPCRequest(data, mempool, redis, metrics) {
    const { method, params, id, user } = data; // 'user' and 'metrics' passed from Gateway
    const startTime = performance.now();

    try {
        let result;
        switch (method) {
            
            // --- IDENTITY LAYER ---
            case "medor_login":
                if (!Array.isArray(params) || params.length < 2) 
                    throw { code: -32602, message: "Invalid login params" };
                result = await verifyUser(params[0], params[1], findUserInDB);
                break;

            // --- CONSENSUS LAYER ---
            case "sendRawTransaction":
                // Auth Check: Tx broadcast requires a valid session
                if (!user) throw { code: -32001, message: "Unauthorized: Session Required" };
                
                const rawTx = TxSchema.parse(params[0]);
                const txRes = await mempool.addTransaction(rawTx);
                
                if (!txRes.ok) {
                    metrics?.increment("mdc_tx_rejections_total", { reason: txRes.reason });
                    throw { code: -32002, message: txRes.reason };
                }
                
                metrics?.increment("mdc_tx_broadcast_total");
                result = txRes.hash;
                break;

            // --- BRIDGE LAYER (BTC / ETH / TUSD) ---
            case "bridge_withdraw":
                // RBAC Check: Ensure user has bridge permissions
                if (!user || user.role !== "USER") 
                    throw { code: -32001, message: "Unauthorized: Insufficient bridge privileges" };

                // Schema Validation
                const [amount, asset, dest] = BridgeSchema.parse(params);

                // Distributed Lock: Prevent concurrent bridge requests per user
                const lockKey = `bridge_lock:${user.sub}`;
                const acquired = await redis.set(lockKey, "LOCKED", "NX", "EX", 30);
                if (!acquired) throw { code: -32005, message: "BRIDGE_BUSY: Existing lock active" };

                try {
                    // Atomic C++ Handoff with 3-attempt retry loop
                    let lockRes;
                    for (let i = 0; i < 3; i++) {
                        lockRes = await mempool.addon.bridge_lockUTXO(amount, asset, dest);
                        if (lockRes.success) break;
                        if (i < 2) await new Promise(r => setTimeout(r, 100)); // 100ms backoff
                    }
                    
                    if (!lockRes || !lockRes.success) {
                        metrics?.increment("mdc_bridge_failures_total", { asset });
                        throw { code: -32000, message: lockRes?.error || "C++_BRIDGE_LOCK_STALL" };
                    }
                    
                    metrics?.increment("mdc_bridge_calls_total", { asset });
                    logger.info("BRIDGE", `Initiated ${amount} ${asset} transfer to ${dest.substring(0,6)}...`);
                    
                    result = { status: "INITIATED", txHash: lockRes.txid, asset };
                } finally {
                    await redis.del(lockKey); // Always release the lock
                }
                break;

            // --- QUERY LAYER ---
            case "getMiningInfo":
                result = await mempool.getMiningTemplate();
                break;

            case "eth_blockNumber":
                result = `0x${mempool.utxoSet.getHeight().toString(16)}`;
                break;

            default:
                throw { code: -32601, message: `Method not found: ${method}` };
        }

        // Record successful latency
        metrics?.observe("mdc_rpc_latency_ms", performance.now() - startTime, { method });
        return result;

    } catch (err) {
        // Standardized JSON-RPC 2.0 Error Mapping
        const rpcError = {
            code: err.code || (err instanceof z.ZodError ? -32602 : -32603),
            message: err.message || "Internal Node Error",
            data: err instanceof z.ZodError ? err.errors : undefined
        };
        
        logger.error("RPC_EXEC_FAIL", `Method: ${method} | Error: ${rpcError.message}`);
        throw rpcError;
    }
}
