#include "receipt.h"
#include "blockchain.h"

TransactionReceipt buildReceipt(
    const Transaction &tx,
    const std::string &blockHash,
    uint64_t blockNumber,
    uint64_t txIndex,
    uint64_t cumulativeGas,
    uint64_t gasUsed,
    bool success,
    const std::vector<ReceiptLog> &logs
) {
    TransactionReceipt r;
    r.transactionHash = tx.txHash;
    r.blockHash = blockHash;
    r.blockNumber = blockNumber;
    r.transactionIndex = txIndex;
    // copy from/to
    std::copy(tx.fromAddress.begin(), tx.fromAddress.end(), r.from.begin());
    std::copy(tx.toAddress.begin(), tx.toAddress.end(), r.to.begin());
    r.gasUsed = gasUsed;
    r.cumulativeGasUsed = cumulativeGas;
    r.logs = logs;
    r.status = success;

    // Compute logsBloom — Bloom filter from all log topics & addresses
    // (256‑byte bloom; 3 bits per topic + address)
    std::fill(r.logsBloom.begin(), r.logsBloom.end(), 0);
    for (auto &log : logs) {
        // update bloom with address & topics
        updateBloom(r.logsBloom, log.address);
        for (auto &topic : log.topics) {
            updateBloom(r.logsBloom, topic);
        }
    }

    return r;
}
