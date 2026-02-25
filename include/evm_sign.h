#pragma once

#include <vector>
#include "transaction.h"
#include <string>

std::vector<uint8_t> signEvmTransaction(
    Transaction &tx,
    const std::string &privKeyPath,
    uint64_t chainId
);
