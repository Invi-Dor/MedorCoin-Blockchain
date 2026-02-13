#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <array>

// A single event log entry
struct ReceiptLog {
    std::array<uint8_t,20> address;       // contract address emitting log
    std::vector<std::array<uint8_t,32>> topics; // indexed event signature & topics
    std::vector<uint8_t> data;            // event data
};

// A transaction receipt following EVM conventions
struct TransactionReceipt {
    std::string transactionHash;     // TX ID
    std::string blockHash;           // containing block
    uint64_t blockNumber = 0;        // block height
    uint64_t transactionIndex = 0;   // index in block
    std::array<uint8_t,20> from;     // sender address
    std::array<uint8_t,20> to;       // recipient or empty for create
    uint64_t cumulativeGasUsed = 0;  // total gas used up to this tx in block
    uint64_t gasUsed = 0;            // gas used by this tx
    std::vector<ReceiptLog> logs;    // event logs emitted
    std::array<uint8_t,256> logsBloom; // bloom filter for logs
    bool status = true;              // true=success, false=fail
};
