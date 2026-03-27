/**
 * Fix #4: SPV Proof Generation
 * Returns the list of hashes required for the bridge relayer to verify a TX.
 */
std::vector<UInt256> MerkleTree::getProof(size_t index) const {
    if (index >= leaves_.size()) {
        throw SerializationError(SerializationErrorCode::LengthExceeded, "Proof index OOB");
    }

    std::vector<UInt256> proof;
    std::vector<UInt256> currentLevel = leaves_;

    while (currentLevel.size() > 1) {
        size_t pairIndex = (index % 2 == 0) ? index + 1 : index - 1;
        
        if (pairIndex < currentLevel.size()) {
            proof.push_back(currentLevel[pairIndex]);
        } else {
            // Bitcoin-style: duplicate the node if it's an odd layer
            proof.push_back(currentLevel[index]);
        }

        index /= 2;
        std::vector<UInt256> nextLevel;
        nextLevel.reserve((currentLevel.size() + 1) / 2);

        for (size_t i = 0; i < currentLevel.size(); i += 2) {
            const UInt256& left = currentLevel[i];
            const UInt256& right = (i + 1 < currentLevel.size()) ? currentLevel[i + 1] : left;
            
            UInt256 combined;
            alignas(16) uint8_t buf[64];
            std::memcpy(buf, left.data(), 32);
            std::memcpy(buf + 32, right.data(), 32);
            
            uint8_t mid[32];
            crypto::doubleSHA256(buf, 64, mid);
            crypto::doubleSHA256(mid, 32, combined.data());
            nextLevel.push_back(combined);
        }
        currentLevel = std::move(nextLevel);
    }
    return proof;
}

/**
 * Fix #10: Parallelized Folding
 * Only triggers if the block size warrants the overhead of context switching.
 */
UInt256 MerkleTree::computeIterative(std::vector<UInt256> layer, ThreadPool* pool) {
    while (layer.size() > 1) {
        size_t nextSize = (layer.size() + 1) / 2;
        std::vector<UInt256> nextLayer(nextSize);

        if (pool && layer.size() > 1024) { // Production Threshold
            std::vector<std::future<void>> futures;
            for (size_t i = 0; i < layer.size(); i += 2) {
                futures.push_back(pool->submit([&layer, &nextLayer, i]() {
                    const UInt256& left = layer[i];
                    const UInt256& right = (i + 1 < layer.size()) ? layer[i + 1] : left;
                    
                    uint8_t buffer[64], mid[32];
                    std::memcpy(buffer, left.data(), 32);
                    std::memcpy(buffer + 32, right.data(), 32);
                    crypto::doubleSHA256(buffer, 64, mid);
                    crypto::doubleSHA256(mid, 32, nextLayer[i / 2].data());
                }));
            }
            for (auto& f : futures) f.get();
        } else {
            // Standard serial path
            for (size_t i = 0; i < layer.size(); i += 2) {
                const UInt256& left = layer[i];
                const UInt256& right = (i + 1 < layer.size()) ? layer[i + 1] : left;
                uint8_t buffer[64], mid[32];
                std::memcpy(buffer, left.data(), 32);
                std::memcpy(buffer + 32, right.data(), 32);
                crypto::doubleSHA256(buffer, 64, mid);
                crypto::doubleSHA256(mid, 32, nextLayer[i / 2].data());
            }
        }
        layer = std::move(nextLayer);
    }
    return layer[0];
}
