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
    r.transactionHash     = tx.txHash;
    r.blockHash           = blockHash;
    r.blockNumber         = blockNumber;
    r.transactionIndex    = txIndex;
    r.from                = "";
    r.to                  = tx.toAddress;
    r.gasUsed             = gasUsed;
    r.cumulativeGasUsed   = cumulativeGas;
    r.logs                = logs;
    r.status              = success;

    std::fill(r.logsBloom.begin(), r.logsBloom.end(), 0);
    for (const auto &log : logs) {
        updateBloom(r.logsBloom, log.address);
        for (const auto &topic : log.topics)
            updateBloom(r.logsBloom, topic);
    }

    return r;
}
