void Blockchain::addBlock(const std::string& minerAddress,
                          std::vector<Transaction>& transactions) {

    // 1. Calculate block reward
    uint64_t reward = calculateReward();
    if (totalSupply + reward > maxSupply) {
        reward = maxSupply - totalSupply;
    }

    // 2. Create coinbase transaction (block reward)
    Transaction coinbaseTx;
    TxOutput minerOut;
    TxOutput ownerOut;

    uint64_t minerReward = (reward * 90) / 100;
    uint64_t ownerReward = reward - minerReward;

    minerOut.value = minerReward;
    minerOut.address = minerAddress;

    ownerOut.value = ownerReward;
    ownerOut.address = ownerAddress;

    coinbaseTx.outputs.push_back(minerOut);
    coinbaseTx.outputs.push_back(ownerOut);
    coinbaseTx.calculateHash();

    // 3. Insert coinbase transaction at the start
    transactions.insert(transactions.begin(), coinbaseTx);

    // 4. Create new block
    Block newBlock(chain.back().hash, "MedorCoin Block", medor, minerAddress);
    newBlock.timestamp = time(nullptr);
    newBlock.reward = reward;
    newBlock.transactions = transactions;

    // 5. Mine the block (lightweight PoW)
    mineBlock(newBlock);

    // 6. Add block to chain
    chain.push_back(newBlock);

    // 7. Update UTXO set (THIS IS THE PART YOU ASKED ABOUT)
    for (auto& tx : newBlock.transactions) {

        // Spend inputs
        for (auto& in : tx.inputs) {
            utxoSet.spendUTXO(in.prevTxHash, in.outputIndex);
        }

        // Create new UTXOs from outputs
        for (size_t i = 0; i < tx.outputs.size(); ++i) {
            utxoSet.addUTXO(tx.outputs[i], tx.txHash, static_cast<int>(i));
        }
    }

    // 8. Update total supply
    totalSupply += reward;
}
