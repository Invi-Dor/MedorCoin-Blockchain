/**
 * FILE: medorcoin-node/services/relayer.cjs
 * INDUSTRIAL VERSION: High-Availability Bridge Relayer
 */
import { findUserInDB } from "./db_service.cjs";
import Redis from "ioredis";
import logger from "../utils/logger.cjs";
import { z } from "zod"; // Fixes Risk #7: Address/Amount Validation

const redis = new Redis(process.env.REDIS_URL || "redis://localhost:6379");

// Validation Schemas per chain (Risk #7 Fix)
const AddressSchema = {
    "BTC": z.string().regex(/^(1|3|bc1)[a-zA-HJ-NP-Z0-9]{25,62}$/),
    "ETH": z.string().regex(/^0x[a-fA-F0-9]{40}$/),
    "TUSD": z.string().regex(/^0x[a-fA-F0-9]{40}$/)
};

export class BridgeRelayer {
    constructor(cppAddon, externalVault) {
        this.addon = cppAddon;
        this.vault = externalVault;
        this.isPolling = false;
    }

    async pollAndExecute() {
        if (this.isPolling) return;
        this.isPolling = true;

        try {
            // 1. Get all pending bridge requests from C++ Core
            const pendingSwaps = await this.addon.bridge_getSwapLogs();

            for (const swap of pendingSwaps) {
                if (swap.status === "LOCKED_PENDING") {
                    // Risk #3 Fix: Distributed Lock (Mutex)
                    // Prevents multiple relayer instances from processing the same swap
                    const lockKey = `lock:bridge:${swap.txHash}`;
                    const acquired = await redis.set(lockKey, "processing", "NX", "EX", 300); // 5 min lock

                    if (acquired) {
                        await this.executeWithRetry(swap);
                    }
                }
            }
        } catch (e) {
            logger.error("RELAYER_POLL_ERROR", e.message);
        } finally {
            this.isPolling = false;
        }
    }

    async executeWithRetry(swap, attempt = 1) {
        const { amount, targetAsset, destAddr, txHash } = swap;

        try {
            // Risk #7 Fix: Pre-flight Validation
            if (!AddressSchema[targetAsset].safeParse(destAddr).success) {
                throw new Error(`INVALID_DESTINATION_ADDRESS_FOR_${targetAsset}`);
            }

            logger.info("RELAYER", `Processing ${amount} ${targetAsset} to ${destAddr} (Attempt ${attempt})`);

            // Risk #5 Fix: Throttling / Rate-Limiting the Vault call
            await new Promise(r => setTimeout(r, 1000)); // 1s safety delay

            // Risk #4 Fix: Call Vault (API/HSM)
            const receipt = await this.vault.sendTransaction(targetAsset, amount, destAddr);

            if (receipt.success) {
                // Risk #1 Fix: Persistence - Mark as COMPLETED in C++ Core
                await this.addon.bridge_completeSwap(txHash, receipt.externalTxid);
                
                // Clear Mutex
                await redis.del(`lock:bridge:${txHash}`);
                
                logger.info("RELAYER_SUCCESS", `Bridged: ${txHash} -> ${receipt.externalTxid}`);
            } else {
                throw new Error(receipt.error || "VAULT_REJECTION");
            }

        } catch (e) {
            // Risk #2 & #6 Fix: Exponential Backoff Retry & Alerting
            if (attempt < 5) {
                const delay = Math.pow(2, attempt) * 1000;
                logger.warn("RELAYER_RETRY", `Swap ${txHash} failed: ${e.message}. Retrying in ${delay}ms...`);
                setTimeout(() => this.executeWithRetry(swap, attempt + 1), delay);
            } else {
                logger.error("RELAYER_FATAL", `Swap ${txHash} FAILED after 5 attempts. Manual intervention required.`);
                // Risk #6: Push alert to Ops (e.g., via Redis Pub/Sub or Webhook)
                await redis.lpush("bridge_alerts", JSON.stringify({ txHash, error: e.message, time: Date.now() }));
            }
        }
    }
}
