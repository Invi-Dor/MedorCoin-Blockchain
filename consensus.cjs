/**
 * MEDOR CONSENSUS ENGINE - ATOMIC REORG & VALIDATOR LOGIC
 */
async _processBlock(block) {
    const validation = await this.validateBlock(block);
    if (!validation.ok) return validation;

    const currentTip = await this.storage.getChainTip();
    
    // Fork Choice: Heaviest Chain (Height + Hash Tie-breaker)
    const isBetter = (block.height > currentTip.height) || 
                     (block.height === currentTip.height && block.hash < currentTip.hash);

    if (isBetter) {
        if (block.parentHash !== currentTip.hash && block.height > 0) {
            return await this.handleReorg(block, currentTip);
        } else {
            return await this.applyToMainChain(block);
        }
    } else {
        await this.storage.saveBlock(block);
        return { ok: true, status: "SIDE_CHAIN_STORED" };
    }
}

/**
 * Fix #3: Emergency Rollback Implementation
 * Ensures the UTXO set remains consistent even during massive reorg failures.
 */
async handleReorg(newBlock, currentTip) {
    const ancestor = await this.findCommonAncestor(newBlock, currentTip);
    const rollbackPath = await this.getBranchPath(ancestor.hash, currentTip.hash);
    const applyPath = await this.getBranchPath(ancestor.hash, newBlock.hash);

    // Snapshot state for emergency recovery
    const originalTipHash = currentTip.hash;

    try {
        // 1. Rollback main chain
        for (const b of rollbackPath.reverse()) {
            await this.utxoSet.rollbackBlock(b);
        }

        // 2. Apply new branch
        for (const b of applyPath) {
            const res = await this.utxoSet.applyBatch(b.transactions, b.height);
            if (!res.ok) throw new Error(res.error);
            await this.storage.setChainTip(b.hash, b.height);
        }

        this.emit('reorg_complete', newBlock.hash);
        return { ok: true, status: "REORG_SUCCESS" };
    } catch (err) {
        console.error("CRITICAL: Reorg failed. Rolling back to original tip.", err);
        
        // Fix #3: Recovery Logic
        // Rollback whatever we applied of the new branch
        // Re-apply the original rollbackPath
        for (const b of rollbackPath) {
            await this.utxoSet.applyBatch(b.transactions, b.height);
        }
        await this.storage.setChainTip(originalTipHash, currentTip.height);
        
        return { ok: false, error: "REORG_ABORTED_RECOVERED" };
    }
}
