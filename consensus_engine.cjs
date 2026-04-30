/**
 * CONSENSUS_ENGINE.CJS - The Sovereign Law of MedorCoin
 * Logic: Proof-of-Work (PoW) with Retargeting and Reward Halving.
 */

"use strict";

const crypto = require('crypto');

class ConsensusEngine {
  constructor(dbInstance) {
    this.db = dbInstance;

    // --- HARDCODED NETWORK CONSTANTS (Production Grade) ---
    this.GENESIS_HASH = "0000000000000000000000000000000000000000000000000000000000000000";
    this.INITIAL_REWARD = 50n * (10n**18n); // 50 MEDOR per block
    this.HALVING_INTERVAL = 210000;         // Every 210k blocks (Bitcoin style)
    
    this.TARGET_BLOCK_TIME = 600;           // 10 minutes (600 seconds)
    this.RETARGET_INTERVAL = 2016;          // Every 2 weeks (2016 blocks)
    
    // Minimum and Maximum difficulty bounds
    this.MAX_TARGET = BigInt("0x00000000FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
  }

  /**
   * Calculates the block reward based on current height (Halving logic)
   */
  getBlockReward(height) {
    const halvings = Math.floor(height / this.HALVING_INTERVAL);
    if (halvings >= 64) return 0n; // Max supply reached
    return this.INITIAL_REWARD >> BigInt(halvings);
  }

  /**
   * Verifies if a block meets the PoW Target (Real difficulty check)
   */
  verifyPoW(blockHash, difficultyTarget) {
    const hashInt = BigInt(`0x${blockHash}`);
    const target = BigInt(difficultyTarget);
    return hashInt <= target;
  }

  /**
   * Implements the "Longest Chain" Rule (Cumulative Work)
   * Resolves forks by picking the chain with most difficulty.
   */
  isBetterChain(newTotalWork, currentTotalWork) {
    return BigInt(newTotalWork) > BigInt(currentTotalWork);
  }

  /**
   * Difficulty Adjustment Algorithm (Retargeting)
   * Prevents the network from being mined too fast or too slow.
   */
  calculateNextTarget(lastRetargetBlock, lastBlock) {
    // Every RETARGET_INTERVAL blocks, adjust difficulty
    if (lastBlock.height % this.RETARGET_INTERVAL !== 0) {
      return lastBlock.difficultyTarget;
    }

    const actualTime = lastBlock.timestamp - lastRetargetBlock.timestamp;
    const expectedTime = this.RETARGET_INTERVAL * this.TARGET_BLOCK_TIME;

    // Clamp adjustment to 4x or 1/4x to prevent extreme swings (Security)
    let adjustmentFactor = actualTime / expectedTime;
    if (adjustmentFactor < 0.25) adjustmentFactor = 0.25;
    if (adjustmentFactor > 4) adjustmentFactor = 4;

    let newTarget = BigInt(lastBlock.difficultyTarget) * BigInt(Math.floor(adjustmentFactor * 100)) / 1024n;

    // Ensure it never goes easier than Genesis
    if (newTarget > this.MAX_TARGET) newTarget = this.MAX_TARGET;

    return newTarget.toString(16);
  }

  /**
   * Validates a Block Header before execution
   */
  async validateBlockHeader(block, prevBlock) {
    // 1. Check Previous Hash link
    if (block.prevHash !== prevBlock.hash) throw new Error("INVALID_PREV_HASH");

    // 2. Check Timestamp (No future blocks, no past blocks older than median)
    if (block.timestamp <= prevBlock.timestamp) throw new Error("BLOCK_TIMESTAMP_TOO_OLD");
    if (block.timestamp > Date.now() / 1000 + 7200) throw new Error("BLOCK_TIMESTAMP_IN_FUTURE");

    // 3. Verify PoW
    if (!this.verifyPoW(block.hash, block.difficultyTarget)) {
        throw new Error("INSUFFICIENT_WORK");
    }

    return true;
  }
}

module.exports = ConsensusEngine;
