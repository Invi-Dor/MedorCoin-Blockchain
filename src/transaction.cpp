#include "transaction.h"
#include "crypto.h"
#include <sstream>

Transaction::Transaction(const std::vector<TxInput>& in, const std::vector<TxOutput>& out)
    : inputs(in), outputs(out) {
    txid = computeTxID();
}

std::string Transaction::computeTxID() {
    std::stringstream ss;
    for (auto &input : inputs) {
        ss << input.prevTxHash << input.outputIndex << input.signature;
    }
    for (auto &output : outputs) {
        ss << output.value << output.recipient;
    }
    return doubleSHA256(ss.str());
}
