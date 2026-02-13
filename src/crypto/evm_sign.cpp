#include "evm_sign.h"
#include "rlp.h"
#include "keccak/keccak.h"
#include "signature.h"

#include <array>

std::vector<uint8_t> signEvmTransaction(
    Transaction &tx,
    const std::string &privKeyPath
) {
    using namespace rlp;

    // ---------- STEP 1: RLP encode unsigned tx ----------

    auto encNonce    = encodeUInt(tx.nonce);
    auto encGasLimit = encodeUInt(tx.gasLimit);
    auto encMaxFee   = encodeUInt(tx.maxFeePerGas);
    auto encTip      = encodeUInt(tx.maxPriorityFeePerGas);

    std::vector<uint8_t> toVec(tx.toAddress.begin(), tx.toAddress.end());
    auto encTo = encodeBytes(toVec);

    auto encValue = encodeUInt(tx.value);
    auto encData  = encodeBytes(tx.data);

    // For signing, v/r/s are empty
    auto encV = encodeUInt(0);
    auto encR = encodeUInt(0);
    auto encS = encodeUInt(0);

    std::vector<std::vector<uint8_t>> items = {
        encNonce,
        encMaxFee,
        encTip,
        encGasLimit,
        encTo,
        encValue,
        encData,
        encV,
        encR,
        encS
    };

    std::vector<uint8_t> unsignedRlp = encodeList(items);

    // ---------- STEP 2: Keccak-256 hash ----------

    uint8_t hash[32];
    keccak(unsignedRlp.data(), unsignedRlp.size(), hash, 32);

    // ---------- STEP 3: Sign hash ----------

    auto sig = signHash(hash, privKeyPath);

    // signHash must return:
    // struct { std::array<uint8_t,32> r; std::array<uint8_t,32> s; uint8_t v; }

    tx.r = sig.r;
    tx.s = sig.s;
    tx.v = sig.v;

    // ---------- STEP 4: RLP encode signed tx ----------

    auto encFinalV = encodeUInt(tx.v);
    auto encFinalR = encodeBytes(std::vector<uint8_t>(tx.r.begin(), tx.r.end()));
    auto encFinalS = encodeBytes(std::vector<uint8_t>(tx.s.begin(), tx.s.end()));

    std::vector<std::vector<uint8_t>> finalItems = {
        encNonce,
        encMaxFee,
        encTip,
        encGasLimit,
        encTo,
        encValue,
        encData,
        encFinalV,
        encFinalR,
        encFinalS
    };

    std::vector<uint8_t> signedRlp = encodeList(finalItems);

    return signedRlp;
}
