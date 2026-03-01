#include "evm_sign.h"
#include "rlp.h"
#include "keccak256.h"
#include "signature.h"

#include <vector>
#include <array>
#include <cstring>

namespace crypto {

static std::vector<uint8_t> buildUnsignedRlp(const EvmTx &tx) {
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
    fields.push_back( encodeUInt(0) );

    return encodeList(fields);
}

std::vector<uint8_t> signEvmTransaction(
    EvmTx &tx,
    const std::array<uint8_t,32> &privkey
) {
    auto unsignedRlp = buildUnsignedRlp(tx);

    auto hashVec = crypto::Keccak256(unsignedRlp);
    std::array<uint8_t,32> hash{};
    std::copy(hashVec.begin(), hashVec.end(), hash.begin());

    auto [rArr, sArr, v] = crypto::signHash(hash, privkey);

    tx.r.assign(rArr.begin(), rArr.end());
    tx.s.assign(sArr.begin(), sArr.end());
    tx.v = static_cast<uint8_t>(v + 27);

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
    finalFields.push_back( encodeUInt(tx.v) );
    finalFields.push_back( encodeBytes(tx.r) );
    finalFields.push_back( encodeBytes(tx.s) );

    return encodeList(finalFields);
}

} // namespace crypto
