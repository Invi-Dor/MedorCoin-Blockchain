#include <gtest/gtest.h>
#include "transaction.h"
#include <array>
#include <thread>
#include <vector>
#include <string>
#include <limits>

// =============================================================================
// HELPERS
// =============================================================================

static Transaction makeTx(uint64_t chainId = 1,
                           uint64_t nonce   = 1,
                           uint64_t value   = 100,
                           const std::string &toAddr = "recipient")
{
    Transaction tx;
    tx.chainId   = chainId;
    tx.nonce     = nonce;
    tx.value     = value;
    tx.toAddress = toAddr;
    tx.gasLimit  = 21000;
    tx.maxFeePerGas         = 10;
    tx.maxPriorityFeePerGas = 2;
    TxOutput out;
    out.value   = value;
    out.address = toAddr;
    tx.outputs.push_back(out);
    tx.calculateHash();
    return tx;
}

// =============================================================================
// DEFAULT FIELDS
// =============================================================================

TEST(TransactionTest, DefaultFields) {
    Transaction tx;
    EXPECT_EQ(tx.chainId,              0ULL);
    EXPECT_EQ(tx.nonce,                0ULL);
    EXPECT_EQ(tx.maxPriorityFeePerGas, 0ULL);
    EXPECT_EQ(tx.maxFeePerGas,         0ULL);
    EXPECT_EQ(tx.gasLimit,             0ULL);
    EXPECT_EQ(tx.value,                0ULL);
    EXPECT_TRUE(tx.txHash.empty());
    EXPECT_TRUE(tx.inputs.empty());
    EXPECT_TRUE(tx.outputs.empty());
    EXPECT_TRUE(tx.toAddress.empty());
    EXPECT_EQ(tx.v, 0ULL);
    EXPECT_EQ(tx.r, (std::array<uint8_t,32>{}));
    EXPECT_EQ(tx.s, (std::array<uint8_t,32>{}));
}

// =============================================================================
// CALCULATE HASH
// =============================================================================

TEST(TransactionTest, CalculateHashProducesNonEmptyHash) {
    Transaction tx;
    tx.chainId   = 1;
    tx.nonce     = 42;
    tx.gasLimit  = 21000;
    tx.value     = 1000;
    tx.toAddress = "0xdeadbeef";
    EXPECT_TRUE(tx.calculateHash());
    EXPECT_FALSE(tx.txHash.empty());
    EXPECT_EQ(tx.txHash.size(), 64U);
}

TEST(TransactionTest, CalculateHashIsDeterministic) {
    Transaction tx1, tx2;
    tx1.chainId   = tx2.chainId   = 1;
    tx1.nonce     = tx2.nonce     = 5;
    tx1.value     = tx2.value     = 500;
    tx1.toAddress = tx2.toAddress = "addr";
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_EQ(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, CalculateHashChangesWithDifferentNonce) {
    Transaction tx1, tx2;
    tx1.nonce = 1;
    tx2.nonce = 2;
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_NE(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, CalculateHashChangesWithDifferentChainId) {
    Transaction tx1, tx2;
    tx1.chainId = 1;
    tx2.chainId = 2;
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_NE(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, CalculateHashChangesWithDifferentValue) {
    Transaction tx1, tx2;
    tx1.value = 100;
    tx2.value = 101;
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_NE(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, CalculateHashChangesWithDifferentRecipient) {
    Transaction tx1, tx2;
    tx1.toAddress = "addrA";
    tx2.toAddress = "addrB";
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_NE(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, CalculateHashChangesWithDifferentGasLimit) {
    Transaction tx1, tx2;
    tx1.gasLimit = 21000;
    tx2.gasLimit = 42000;
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_NE(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, CalculateHashChangesWithDifferentMaxFee) {
    Transaction tx1, tx2;
    tx1.maxFeePerGas = 10;
    tx2.maxFeePerGas = 20;
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_NE(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, CalculateHashChangesWithDifferentPriorityFee) {
    Transaction tx1, tx2;
    tx1.maxPriorityFeePerGas = 1;
    tx2.maxPriorityFeePerGas = 2;
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_NE(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, CalculateHashChangesWithDifferentOutputs) {
    Transaction tx1, tx2;
    TxOutput o1, o2;
    o1.value = 100; o1.address = "addr1";
    o2.value = 200; o2.address = "addr2";
    tx1.outputs.push_back(o1);
    tx2.outputs.push_back(o2);
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_NE(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, CalculateHashChangesWithDifferentInputs) {
    Transaction tx1, tx2;
    TxInput i1, i2;
    i1.prevTxHash = "hash1"; i1.outputIndex = 0;
    i2.prevTxHash = "hash2"; i2.outputIndex = 0;
    tx1.inputs.push_back(i1);
    tx2.inputs.push_back(i2);
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_NE(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, HashIsHexLowercase) {
    Transaction tx = makeTx();
    for (char c : tx.txHash)
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}

TEST(TransactionTest, HashIsAlways64Chars) {
    for (int i = 0; i < 10; ++i) {
        Transaction tx = makeTx(1, static_cast<uint64_t>(i));
        EXPECT_EQ(tx.txHash.size(), 64U);
    }
}

// =============================================================================
// IS VALID
// =============================================================================

TEST(TransactionTest, IsValidReturnsFalseWithoutHash) {
    Transaction tx;
    EXPECT_FALSE(tx.isValid());
}

TEST(TransactionTest, IsValidReturnsTrueAfterHash) {
    Transaction tx;
    tx.chainId = 1;
    tx.nonce   = 1;
    EXPECT_TRUE(tx.calculateHash());
    EXPECT_TRUE(tx.isValid());
}

TEST(TransactionTest, IsValidReturnsFalseAfterHashCleared) {
    Transaction tx = makeTx();
    tx.txHash.clear();
    EXPECT_FALSE(tx.isValid());
}

TEST(TransactionTest, IsValidReturnsFalseWithCorruptedHash) {
    Transaction tx = makeTx();
    tx.txHash[0] = '!';  // not a valid hex char
    EXPECT_FALSE(tx.isValid());
}

// =============================================================================
// TX INPUT
// =============================================================================

TEST(TransactionTest, TxInputDefaultOutputIndex) {
    TxInput in;
    EXPECT_EQ(in.outputIndex, 0);
    EXPECT_TRUE(in.prevTxHash.empty());
}

TEST(TransactionTest, TxInputFields) {
    TxInput in;
    in.prevTxHash  = "abc123";
    in.outputIndex = 2;
    EXPECT_EQ(in.prevTxHash,  "abc123");
    EXPECT_EQ(in.outputIndex, 2);
}

TEST(TransactionTest, TxInputNegativeOutputIndex) {
    TxInput in;
    in.outputIndex = -1;
    EXPECT_EQ(in.outputIndex, -1);
}

TEST(TransactionTest, TxInputLargePrevHash) {
    TxInput in;
    in.prevTxHash = std::string(64, 'f');
    EXPECT_EQ(in.prevTxHash.size(), 64U);
}

// =============================================================================
// TX OUTPUT
// =============================================================================

TEST(TransactionTest, TxOutputDefaultValue) {
    TxOutput out;
    EXPECT_EQ(out.value, 0ULL);
    EXPECT_TRUE(out.address.empty());
}

TEST(TransactionTest, TxOutputFields) {
    TxOutput out;
    out.value   = 1000ULL;
    out.address = "medor1addr";
    EXPECT_EQ(out.value,   1000ULL);
    EXPECT_EQ(out.address, "medor1addr");
}

TEST(TransactionTest, TxOutputZeroValueIsValid) {
    TxOutput out;
    out.value   = 0ULL;
    out.address = "addr";
    EXPECT_EQ(out.value, 0ULL);
}

TEST(TransactionTest, TxOutputMaxUint64Value) {
    TxOutput out;
    out.value   = std::numeric_limits<uint64_t>::max();
    out.address = "addr";
    EXPECT_EQ(out.value, std::numeric_limits<uint64_t>::max());
}

// =============================================================================
// MULTIPLE INPUTS AND OUTPUTS
// =============================================================================

TEST(TransactionTest, MultipleInputsAndOutputs) {
    Transaction tx;
    TxInput in1, in2;
    in1.prevTxHash  = "hash1"; in1.outputIndex = 0;
    in2.prevTxHash  = "hash2"; in2.outputIndex = 1;
    tx.inputs.push_back(in1);
    tx.inputs.push_back(in2);

    TxOutput out1, out2;
    out1.value = 500; out1.address = "addr1";
    out2.value = 400; out2.address = "addr2";
    tx.outputs.push_back(out1);
    tx.outputs.push_back(out2);

    EXPECT_EQ(tx.inputs.size(),        2U);
    EXPECT_EQ(tx.outputs.size(),       2U);
    EXPECT_EQ(tx.inputs[0].prevTxHash, "hash1");
    EXPECT_EQ(tx.outputs[1].value,     400ULL);
}

TEST(TransactionTest, ManyInputsManyOutputs) {
    Transaction tx;
    for (int i = 0; i < 100; ++i) {
        TxInput in;
        in.prevTxHash  = "hash" + std::to_string(i);
        in.outputIndex = i;
        tx.inputs.push_back(in);

        TxOutput out;
        out.value   = static_cast<uint64_t>(i * 10);
        out.address = "addr" + std::to_string(i);
        tx.outputs.push_back(out);
    }
    EXPECT_EQ(tx.inputs.size(),  100U);
    EXPECT_EQ(tx.outputs.size(), 100U);
    EXPECT_TRUE(tx.calculateHash());
}

TEST(TransactionTest, NoInputsCoinbaseLike) {
    Transaction tx;
    tx.chainId = 1;
    TxOutput out;
    out.value   = 5500;
    out.address = "miner";
    tx.outputs.push_back(out);
    EXPECT_TRUE(tx.calculateHash());
    EXPECT_TRUE(tx.isValid());
    EXPECT_TRUE(tx.inputs.empty());
}

TEST(TransactionTest, NoOutputsIsValidStructurally) {
    Transaction tx;
    tx.chainId = 1;
    tx.nonce   = 1;
    TxInput in;
    in.prevTxHash  = "prev";
    in.outputIndex = 0;
    tx.inputs.push_back(in);
    EXPECT_TRUE(tx.calculateHash());
}

// =============================================================================
// SIGNATURE FIELDS
// =============================================================================

TEST(TransactionTest, SignatureFieldsDefaultZero) {
    Transaction tx;
    for (uint8_t b : tx.r) EXPECT_EQ(b, 0);
    for (uint8_t b : tx.s) EXPECT_EQ(b, 0);
    EXPECT_EQ(tx.v, 0ULL);
}

TEST(TransactionTest, SignatureFieldsCanBeSet) {
    Transaction tx;
    tx.r.fill(0xAB);
    tx.s.fill(0xCD);
    tx.v = 27;
    for (uint8_t b : tx.r) EXPECT_EQ(b, 0xAB);
    for (uint8_t b : tx.s) EXPECT_EQ(b, 0xCD);
    EXPECT_EQ(tx.v, 27ULL);
}

TEST(TransactionTest, SignatureDoesNotAffectHashComputation) {
    Transaction tx1 = makeTx();
    Transaction tx2 = makeTx();
    tx2.r.fill(0xFF);
    tx2.s.fill(0xFF);
    tx2.v = 28;
    tx2.calculateHash();
    // hash should be the same since signature is not part of pre-sign hash
    EXPECT_EQ(tx1.txHash, tx2.txHash);
}

// =============================================================================
// OPERATIONAL FIELD RANGES
// =============================================================================

TEST(TransactionTest, MaxUint64ChainId) {
    Transaction tx;
    tx.chainId = std::numeric_limits<uint64_t>::max();
    EXPECT_TRUE(tx.calculateHash());
    EXPECT_FALSE(tx.txHash.empty());
}

TEST(TransactionTest, MaxUint64Nonce) {
    Transaction tx;
    tx.nonce = std::numeric_limits<uint64_t>::max();
    EXPECT_TRUE(tx.calculateHash());
}

TEST(TransactionTest, MaxUint64GasLimit) {
    Transaction tx;
    tx.gasLimit = std::numeric_limits<uint64_t>::max();
    EXPECT_TRUE(tx.calculateHash());
}

TEST(TransactionTest, MaxUint64Value) {
    Transaction tx;
    tx.value = std::numeric_limits<uint64_t>::max();
    EXPECT_TRUE(tx.calculateHash());
}

TEST(TransactionTest, MaxUint64MaxFeePerGas) {
    Transaction tx;
    tx.maxFeePerGas = std::numeric_limits<uint64_t>::max();
    EXPECT_TRUE(tx.calculateHash());
}

TEST(TransactionTest, ZeroGasLimitIsValid) {
    Transaction tx;
    tx.gasLimit = 0;
    EXPECT_TRUE(tx.calculateHash());
}

TEST(TransactionTest, ZeroValueIsValid) {
    Transaction tx;
    tx.value = 0;
    EXPECT_TRUE(tx.calculateHash());
}

// =============================================================================
// DATA FIELD
// =============================================================================

TEST(TransactionTest, DataFieldDefaultEmpty) {
    Transaction tx;
    EXPECT_TRUE(tx.data.empty());
}

TEST(TransactionTest, DataFieldCanBeSet) {
    Transaction tx;
    tx.data = { 0x01, 0x02, 0x03 };
    EXPECT_EQ(tx.data.size(), 3U);
    EXPECT_EQ(tx.data[0], 0x01);
}

TEST(TransactionTest, DataFieldAffectsHash) {
    Transaction tx1, tx2;
    tx1.data = { 0x01 };
    tx2.data = { 0x02 };
    EXPECT_TRUE(tx1.calculateHash());
    EXPECT_TRUE(tx2.calculateHash());
    EXPECT_NE(tx1.txHash, tx2.txHash);
}

TEST(TransactionTest, LargeDataField) {
    Transaction tx;
    tx.data.resize(10000, 0xAB);
    EXPECT_TRUE(tx.calculateHash());
    EXPECT_EQ(tx.txHash.size(), 64U);
}

// =============================================================================
// CONCURRENCY
// =============================================================================

TEST(TransactionTest, ConcurrentHashCalculationIsSafe) {
    constexpr int THREADS = 8;
    std::vector<std::thread> threads;
    std::vector<std::string> hashes(THREADS);

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&hashes, i]() {
            Transaction tx;
            tx.chainId = 1;
            tx.nonce   = static_cast<uint64_t>(i);
            tx.value   = 100;
            tx.calculateHash();
            hashes[i] = tx.txHash;
        });
    }
    for (auto &t : threads) t.join();

    // Each thread had a different nonce so all hashes must be different
    for (int i = 0; i < THREADS; ++i)
        for (int j = i + 1; j < THREADS; ++j)
            EXPECT_NE(hashes[i], hashes[j]);
}

TEST(TransactionTest, ConcurrentIsValidReadIsSafe) {
    Transaction tx = makeTx();
    constexpr int THREADS = 16;
    std::vector<std::thread> threads;
    std::vector<bool> results(THREADS);

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&tx, &results, i]() {
            results[i] = tx.isValid();
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_TRUE(results[i]);
}

TEST(TransactionTest, ConcurrentDeterministicHashMatch) {
    // All threads hash the same tx -- all results must be identical
    constexpr int THREADS = 8;
    std::vector<std::thread> threads;
    std::vector<std::string> hashes(THREADS);
    std::string reference;
    {
        Transaction ref;
        ref.chainId = 1; ref.nonce = 99; ref.value = 500;
        ref.calculateHash();
        reference = ref.txHash;
    }

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&hashes, i]() {
            Transaction tx;
            tx.chainId = 1; tx.nonce = 99; tx.value = 500;
            tx.calculateHash();
            hashes[i] = tx.txHash;
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_EQ(hashes[i], reference);
}
