void UTXOSet::removeAddressIndexSafe(const std::string& address, const std::string& utxoKey) {
    if (address.empty() || utxoKey.empty()) return;

    // Use a separate shard lock based on Address to prevent cross-shard deadlocks
    size_t addressShardIdx = std::hash<std::string>{}(address) % NUM_SHARDS;
    std::unique_lock<std::shared_mutex> addressLock(addressIndexMutexes_[addressShardIdx]);
    
    // 1. Address Existence Check
    // Use .find() to avoid the [] operator, which creates an entry even if it doesn't exist.
    auto& shardMap = addressToUtxoIndex_[addressShardIdx];
    auto addrIt = shardMap.find(address);
    
    if (addrIt != shardMap.end()) {
        auto& utxoSet = addrIt->second;
        
        // 3. Log removals for debugging (Level 1/Info)
        if (utxoSet.erase(utxoKey) > 0) {
            slog(LOG_INFO, "IndexRemove: Erased UTXO " + utxoKey + " from address " + address);
            
            // Optional: Update a per-address metric if you track those
            // metrics_.activeAddressesCount.fetch_sub(1, std::memory_order_relaxed);
        }

        // 2. Empty Map Cleanup
        // If this was the last UTXO for this address, remove the address entry entirely.
        // This prevents memory exhaustion over millions of transactions.
        if (utxoSet.empty()) {
            shardMap.erase(addrIt);
            slog(LOG_INFO, "IndexRemove: Address " + address + " now empty, cleared from RAM.");
        }
    } else {
        // This should technically not happen if the UTXO exists in the main shard,
        // but it's a critical safety check for production.
        slog(LOG_WARNING, "IndexRemove: Attempted to remove UTXO from non-existent address index: " + address);
    }
}
