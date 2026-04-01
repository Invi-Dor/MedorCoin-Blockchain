utXo.cpp
// =============================================================================
// MODIFIED SPEND UTXO (Ensures locked UTXOs cannot be spent natively)
// =============================================================================
bool UTXOSet::spendUTXO(const std::string& txHash,
                          int                outputIndex,
                          uint64_t           currentBlockHeight) noexcept
{
    if (txHash.empty()) {
        slog(2, "spendUTXO: empty txHash rejected");
        return false;
    }
    if (outputIndex < 0) {
        slog(2, "spendUTXO: negative outputIndex rejected");
        return false;
    }

    const std::string key = makeKey(txHash, outputIndex);

    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    auto it = utxos_.find(key);
    if (it == utxos_.end()) {
        slog(1, "spendUTXO: key '" + key
                + "' not found (already spent or nonexistent)");
        return false;
    }

    const UTXO& utxo = it->second;

    // CRITICAL SECURITY CHECK: Prevent spending of bridged/locked UTXOs
    if (utxo.isLocked) {
        slog(2, "spendUTXO: key '" + key + "' rejected - currently locked for bridge");
        return false;
    }

    // Coinbase maturity enforcement
    if (utxo.isCoinbase &&
        currentBlockHeight != std::numeric_limits<uint64_t>::max())
    {
        if (currentBlockHeight < utxo.blockHeight + COINBASE_MATURITY) {
            slog(1, "spendUTXO: coinbase not matured key='" + key + "'");
            metrics_.coinbaseMaturityRejected.fetch_add(
                1, std::memory_order_relaxed);
            return false;
        }
    }

    metrics_.totalValueTracked.fetch_sub(utxo.value,
        std::memory_order_relaxed);
    metrics_.utxosSpent.fetch_add(1, std::memory_order_relaxed);

    indexRemove(utxo.address, key);
    utxos_.erase(it);

    persist(); // called while write lock is held
    return true;
}

// =============================================================================
// LOCK UTXO (Cross-Chain Wrap Initiation)
// =============================================================================
bool UTXOSet::lockUTXO(const std::string& txHash, int outputIndex, const std::string& evmAddress) noexcept {
    if (txHash.empty() || outputIndex < 0 || evmAddress.empty()) {
        slog(2, "lockUTXO: invalid parameters");
        return false;
    }

    const std::string key = makeKey(txHash, outputIndex);
    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    auto it = utxos_.find(key);
    if (it == utxos_.end()) {
        slog(2, "lockUTXO: key '" + key + "' not found");
        return false;
    }

    if (it->second.isLocked) {
        slog(2, "lockUTXO: key '" + key + "' is already locked");
        return false;
    }

    it->second.isLocked = true;
    it->second.lockedForAddress = evmAddress;
    
    // Log the event explicitly so the .cjs relayer can pick it up via RPC/ZMQ
    slog(0, "WRAP_REQUEST|" + key + "|" + std::to_string(it->second.value) + "|" + evmAddress);
    
    persist();
    return true;
}

// =============================================================================
// UNLOCK UTXO (Cross-Chain Unwrap Resolution)
// =============================================================================
bool UTXOSet::unlockUTXO(const std::string& txHash, int outputIndex) noexcept {
    if (txHash.empty() || outputIndex < 0) {
        slog(2, "unlockUTXO: invalid parameters");
        return false;
    }

    const std::string key = makeKey(txHash, outputIndex);
    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    auto it = utxos_.find(key);
    if (it == utxos_.end()) {
        slog(2, "unlockUTXO: key '" + key + "' not found");
        return false;
    }

    if (!it->second.isLocked) {
        slog(2, "unlockUTXO: key '" + key + "' is not locked");
        return false;
    }

    it->second.isLocked = false;
    it->second.lockedForAddress = "";
    
    slog(0, "UNWRAP_SUCCESS|" + key);
    
    persist();
    return true;
}
