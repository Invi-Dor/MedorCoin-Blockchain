#include "crypto/evm_sign.h"
#include "crypto/rlp.h"
#include "crypto/keccak256.h"
#include "crypto/signature.h"

#include <array>
#include <cstring>
#include <vector>

namespace crypto {

static std::vector<uint8_t> buildUnsignedRlp(const EvmTx &tx)
{
    std::vector<uint8_t> chainId, nonce, maxPriority, maxFee, gasLimit,
                         to, value, data, accessList;

    rlp::encodeUInt(tx.chainId,              chainId);
    rlp::encodeUInt(tx.nonce,                nonce);
    rlp::encodeUInt(tx.maxPriorityFeePerGas, maxPriority);
    rlp::encodeUInt(tx.maxFeePerGas,         maxFee);
    rlp::encodeUInt(tx.gasLimit,             gasLimit);
    rlp::encodeBytes(tx.to,                  to);
    rlp::encodeBytes(tx.value,               value);
    rlp::encodeBytes(tx.data,                data);
    rlp::encodeUInt(0,                       accessList);  // empty access list

    std::vector<std::vector<uint8_t>> fields = {
        chainId, nonce, maxPriority, maxFee,
        gasLimit, to, value, data, accessList
    };

    std::vector<uint8_t> out;
    rlp::encodeList(fields, out);
    return out;
}

std::vector<uint8_t> signEvmTransaction(
    EvmTx                        &tx,
    const std::array<uint8_t,32> &privkey)
{
    auto unsignedRlp = buildUnsignedRlp(tx);

    auto hashVec = crypto::Keccak256(unsignedRlp);
    std::array<uint8_t, 32> hash{};
    std::copy(hashVec.begin(), hashVec.end(), hash.begin());

    auto [rArr, sArr, v] = signHashWithKey(hash, privkey);

    tx.r.assign(rArr.begin(), rArr.end());
    tx.s.assign(sArr.begin(), sArr.end());
    tx.v = static_cast<uint8_t>(v + 27);

    std::vector<uint8_t> chainId, nonce, maxPriority, maxFee, gasLimit,
                         to, value, data, vEnc, r, s, accessList;

    rlp::encodeUInt(tx.chainId,              chainId);
    rlp::encodeUInt(tx.nonce,                nonce);
    rlp::encodeUInt(tx.maxPriorityFeePerGas, maxPriority);
    rlp::encodeUInt(tx.maxFeePerGas,         maxFee);
    rlp::encodeUInt(tx.gasLimit,             gasLimit);
    rlp::encodeBytes(tx.to,                  to);
    rlp::encodeBytes(tx.value,               value);
    rlp::encodeBytes(tx.data,                data);
    rlp::encodeUInt(0,                       accessList);
    rlp::encodeUInt(tx.v,                    vEnc);
    rlp::encodeBytes(tx.r,                   r);
    rlp::encodeBytes(tx.s,                   s);

    std::vector<std::vector<uint8_t>> finalFields = {
        chainId, nonce, maxPriority, maxFee,
        gasLimit, to, value, data, accessList,
        vEnc, r, s
    };

    std::vector<uint8_t> out;
    rlp::encodeList(finalFields, out);
    return out;
}

} // namespace crypto
