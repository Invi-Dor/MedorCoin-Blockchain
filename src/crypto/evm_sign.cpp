// File: crypto/evm_sign.cpp
// SPDX‑License‑Identifier: MIT
// Purpose: EVM transaction signing (legacy + EIP‑1559 dynamic fees) using libsecp256k1.
// This produces a signed RLP‑encoded transaction ready for broadcasting.

#include "evm_sign.h"
#include "rlp.h"
#include "keccak256.h"
#include "secp256k1_wrapper.h"
#include "signature_helpers.h"

#include <vector>
#include <array>
#include <stdexcept>

namespace {

// Build the *unsigned* RLP payload for signing (chainId, nonce, fees, to, value, data, empty v,r,s).
// This matches how dynamic fee transactions are hashed for signing.  [oai_citation:1‡console.settlemint.com](https://console.settlemint.com/documentation/blockchain-platform/knowledge-bank/besu-transaction-flow?utm_source=chatgpt.com)
std::vector<uint8_t> buildUnsignedRlp(const EvmTx &tx) {
    using namespace rlp;

    std::vector<std::vector<uint8_t>> fields;
    fields.push_back( encodeUInt(tx.chainId) );
    fields.push_back( encodeUInt(tx.nonce) );
    fields.push_back( encodeUInt(tx.maxPriorityFeePerGas) );
    fields.push_back( encodeUInt(tx.maxFeePerGas) );
    fields.push_back( encodeUInt(tx.gasLimit) );
    fields.push_back( encodeBytes(tx.to) );
    fields.push_back( encodeBytes(tx.value) );
    fields.push_back( encodeBytes(tx.data) );

    // EIP‑1559 typed transactions add an access list, but for simplicity assume none
    fields.push_back( encodeUInt(0) );

    // Placeholder signature fields
    fields.push_back( encodeUInt(0) ); // v placeholder
    fields.push_back( encodeUInt(0) ); // r placeholder
    fields.push_back( encodeUInt(0) ); // s placeholder

    return encodeList(fields);
}

} // namespace

std::vector<uint8_t> signEvmTransaction(
    EvmTx &tx,
    const std::array<uint8_t,32> &privkey
) {
    // 1) Build unsigned RLP payload
    auto unsignedRlp = buildUnsignedRlp(tx);

    // 2) Hash using Keccak256
    auto hashVec = crypto::keccak256(unsignedRlp);
    if (hashVec.size() != 32) {
        throw std::runtime_error("Keccak256 hash must be 32 bytes");
    }
    std::array<uint8_t, 32> hash;
    std::copy(hashVec.begin(), hashVec.end(), hash.begin());

    // 3) Sign the hash recoverably with secp256k1
    auto sigOpt = crypto::signRecoverable(hash.data(), privkey);
    if (!sigOpt) {
        throw std::runtime_error("Secp256k1 recoverable signing failed");
    }
    auto sig = *sigOpt;

    // 4) Populate tx signature fields
    tx.r.assign(sig.r.begin(), sig.r.end());
    tx.s.assign(sig.s.begin(), sig.s.end());
    tx.v = crypto::computeEip155V(sig.recid, tx.chainId);

    // 5) Build the final signed RLP payload
    using namespace rlp;
    std::vector<std::vector<uint8_t>> finalFields;
    finalFields.push_back( encodeUInt(tx.chainId) );
    finalFields.push_back( encodeUInt(tx.nonce) );
    finalFields.push_back( encodeUInt(tx.maxPriorityFeePerGas) );
    finalFields.push_back( encodeUInt(tx.maxFeePerGas) );
    finalFields.push_back( encodeUInt(tx.gasLimit) );
    finalFields.push_back( encodeBytes(tx.to) );
    finalFields.push_back( encodeBytes(tx.value) );
    finalFields.push_back( encodeBytes(tx.data) );

    // Access list (empty)
    finalFields.push_back( encodeUInt(0) );

    // Signature parts
    finalFields.push_back( encodeUInt(tx.v) );
    finalFields.push_back( encodeBytes(tx.r) );
    finalFields.push_back( encodeBytes(tx.s) );

    return encodeList(finalFields);
}
