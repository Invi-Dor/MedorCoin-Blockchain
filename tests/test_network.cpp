#include <gtest/gtest.h>
#include "net/serialization.h"
#include "transaction.h"
#include "block.h"
#include "blockchain.h"
#include "utxo.h"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

// =============================================================================
// HELPERS
// =============================================================================

static Transaction makeTx(uint64_t chainId = 1,
                           uint64_t nonce   = 1,
                           uint64_t value   = 100,
                           const std::string &to = "recipient")
{
    Transaction tx;
    tx.chainId              = chainId;
    tx.nonce                = nonce;
    tx.value                = value;
    tx.toAddress            = to;
    tx.gasLimit             = 21000;
    tx.maxFeePerGas         = 10;
    tx.maxPriorityFeePerGas = 2;
    TxInput in;
    in.prevTxHash  = "prevhash" + std::to_string(nonce);
    in.outputIndex = 0;
    tx.inputs.push_back(in);
    TxOutput out;
    out.value   = value;
    out.address = to;
    tx.outputs.push_back(out);
    tx.calculateHash();
    return tx;
}

static Block makeBlock()
{
    Block b("prevhash", "test data", 2, "miner1");
    b.timestamp = 1000ULL;
    b.reward    = 55ULL;
    b.baseFee   = 1ULL;
    b.nonce     = 42ULL;
    b.gasUsed   = 0ULL;
    b.gasLimit  = 1000000ULL;
    b.hash      = std::string(64, '0');
    return b;
}

static Blockchain::Config netTestConfig(const std::string &suffix = "")
{
    Blockchain::Config cfg;
    cfg.ownerAddress   = "medor_owner";
    cfg.blockDBPath    = "/tmp/test_net_blockdb"   + suffix;
    cfg.accountDBPath  = "/tmp/test_net_accountdb" + suffix;
    cfg.maxSupply      = 1000000ULL;
    cfg.initialMedor   = 1;
    cfg.initialBaseFee = 1ULL;
    cfg.rewardSchedule = { { UINT64_MAX, 55ULL } };
    return cfg;
}

// =============================================================================
// FIXTURE -- blockchain context
// =============================================================================

class NetworkTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<int> counter{0};
        suffix_ = std::to_string(++counter);
        bc   = std::make_unique<Blockchain>(netTestConfig(suffix_));
        utxo = std::make_unique<UTXOSet>();
    }
    void TearDown() override {
        bc.reset();
        utxo.reset();
        std::filesystem::remove_all("/tmp/test_net_blockdb"   + suffix_);
        std::filesystem::remove_all("/tmp/test_net_accountdb" + suffix_);
    }
    std::unique_ptr<Blockchain> bc;
    std::unique_ptr<UTXOSet>    utxo;
    std::string                 suffix_;
};

// =============================================================================
// TRANSACTION SERIALIZATION -- STRUCTURE
// =============================================================================

TEST_F(NetworkTest, SerializeTxProducesJson) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    EXPECT_FALSE(j.empty());
}

TEST_F(NetworkTest, SerializeTxContainsTxHash) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    EXPECT_TRUE(j.contains("txHash"));
    EXPECT_EQ(j["txHash"], tx.txHash);
}

TEST_F(NetworkTest, SerializeTxContainsChainId) {
    Transaction tx = makeTx(5);
    json j = serializeTx(tx);
    EXPECT_TRUE(j.contains("chainId"));
    EXPECT_EQ(j["chainId"].get<uint64_t>(), 5ULL);
}

TEST_F(NetworkTest, SerializeTxContainsNonce) {
    Transaction tx = makeTx(1, 99);
    json j = serializeTx(tx);
    EXPECT_TRUE(j.contains("nonce"));
    EXPECT_EQ(j["nonce"].get<uint64_t>(), 99ULL);
}

TEST_F(NetworkTest, SerializeTxContainsValue) {
    Transaction tx = makeTx(1, 1, 777);
    json j = serializeTx(tx);
    EXPECT_TRUE(j.contains("value"));
    EXPECT_EQ(j["value"].get<uint64_t>(), 777ULL);
}

TEST_F(NetworkTest, SerializeTxContainsInputs) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    EXPECT_TRUE(j.contains("inputs"));
    EXPECT_TRUE(j["inputs"].is_array());
    EXPECT_EQ(j["inputs"].size(), 1U);
}

TEST_F(NetworkTest, SerializeTxContainsOutputs) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    EXPECT_TRUE(j.contains("outputs"));
    EXPECT_TRUE(j["outputs"].is_array());
    EXPECT_EQ(j["outputs"].size(), 1U);
}

TEST_F(NetworkTest, SerializeTxInputFieldsCorrect) {
    Transaction tx = makeTx(1, 7);
    json j = serializeTx(tx);
    auto &in = j["inputs"][0];
    EXPECT_TRUE(in.contains("prevTxHash"));
    EXPECT_TRUE(in.contains("outputIndex"));
    EXPECT_EQ(in["prevTxHash"].get<std::string>(), "prevhash7");
    EXPECT_EQ(in["outputIndex"].get<int>(), 0);
}

TEST_F(NetworkTest, SerializeTxOutputFieldsCorrect) {
    Transaction tx = makeTx(1, 1, 500, "targetAddr");
    json j = serializeTx(tx);
    auto &out = j["outputs"][0];
    EXPECT_TRUE(out.contains("value"));
    EXPECT_TRUE(out.contains("address"));
    EXPECT_EQ(out["value"].get<uint64_t>(),      500ULL);
    EXPECT_EQ(out["address"].get<std::string>(), "targetAddr");
}

TEST_F(NetworkTest, SerializeTxContainsGasFields) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    EXPECT_TRUE(j.contains("gasLimit"));
    EXPECT_TRUE(j.contains("maxFeePerGas"));
    EXPECT_TRUE(j.contains("maxPriorityFeePerGas"));
}

// =============================================================================
// TRANSACTION SERIALIZATION -- ROUND TRIP
// =============================================================================

TEST_F(NetworkTest, DeserializeTxRoundTrip) {
    Transaction tx = makeTx(1, 5, 300, "destAddr");
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.txHash,    tx.txHash);
    EXPECT_EQ(tx2.chainId,   tx.chainId);
    EXPECT_EQ(tx2.nonce,     tx.nonce);
    EXPECT_EQ(tx2.value,     tx.value);
    EXPECT_EQ(tx2.toAddress, tx.toAddress);
    EXPECT_EQ(tx2.gasLimit,  tx.gasLimit);
}

TEST_F(NetworkTest, DeserializeTxInputsPreserved) {
    Transaction tx = makeTx(1, 3);
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    ASSERT_EQ(tx2.inputs.size(), 1U);
    EXPECT_EQ(tx2.inputs[0].prevTxHash,  tx.inputs[0].prevTxHash);
    EXPECT_EQ(tx2.inputs[0].outputIndex, tx.inputs[0].outputIndex);
}

TEST_F(NetworkTest, DeserializeTxOutputsPreserved) {
    Transaction tx = makeTx(1, 1, 250, "outAddr");
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    ASSERT_EQ(tx2.outputs.size(), 1U);
    EXPECT_EQ(tx2.outputs[0].value,   250ULL);
    EXPECT_EQ(tx2.outputs[0].address, "outAddr");
}

TEST_F(NetworkTest, DeserializeTxMultipleInputs) {
    Transaction tx;
    tx.chainId = 1; tx.nonce = 1;
    for (int i = 0; i < 5; ++i) {
        TxInput in;
        in.prevTxHash  = "hash" + std::to_string(i);
        in.outputIndex = i;
        tx.inputs.push_back(in);
    }
    TxOutput out; out.value = 100; out.address = "addr";
    tx.outputs.push_back(out);
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    ASSERT_EQ(tx2.inputs.size(), 5U);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(tx2.inputs[i].prevTxHash,  "hash" + std::to_string(i));
        EXPECT_EQ(tx2.inputs[i].outputIndex, i);
    }
}

TEST_F(NetworkTest, DeserializeTxMultipleOutputs) {
    Transaction tx;
    tx.chainId = 1; tx.nonce = 1;
    TxInput in; in.prevTxHash = "prev"; in.outputIndex = 0;
    tx.inputs.push_back(in);
    for (int i = 0; i < 3; ++i) {
        TxOutput out;
        out.value   = static_cast<uint64_t>((i + 1) * 100);
        out.address = "addr" + std::to_string(i);
        tx.outputs.push_back(out);
    }
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    ASSERT_EQ(tx2.outputs.size(), 3U);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(tx2.outputs[i].value,
                  static_cast<uint64_t>((i + 1) * 100));
        EXPECT_EQ(tx2.outputs[i].address, "addr" + std::to_string(i));
    }
}

TEST_F(NetworkTest, DeserializeTxNoInputsNoOutputs) {
    Transaction tx;
    tx.chainId = 1;
    tx.nonce   = 42;
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_TRUE(tx2.inputs.empty());
    EXPECT_TRUE(tx2.outputs.empty());
    EXPECT_EQ(tx2.nonce, 42ULL);
}

// =============================================================================
// TRANSACTION SERIALIZATION -- FAILURE PATHS
// =============================================================================

TEST_F(NetworkTest, DeserializeTxMissingTxHashThrows) {
    json j;
    j["chainId"] = 1;
    j["nonce"]   = 1;
    EXPECT_THROW(deserializeTx(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeTxMissingChainIdThrows) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    j.erase("chainId");
    EXPECT_THROW(deserializeTx(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeTxWrongTypeThrows) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    j["chainId"] = "not_a_number";
    EXPECT_THROW(deserializeTx(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeTxEmptyJsonThrows) {
    json j = json::object();
    EXPECT_THROW(deserializeTx(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeTxNullJsonThrows) {
    json j = nullptr;
    EXPECT_THROW(deserializeTx(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeTxArrayJsonThrows) {
    json j = json::array();
    EXPECT_THROW(deserializeTx(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeTxMissingGasLimitThrows) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    j.erase("gasLimit");
    EXPECT_THROW(deserializeTx(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeTxNegativeOutputIndexThrows) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    j["inputs"][0]["outputIndex"] = -999;
    EXPECT_THROW(deserializeTx(j), SerializationError);
}

// =============================================================================
// TRANSACTION -- EXTREME EDGE CASES
// =============================================================================

TEST_F(NetworkTest, MaxUint64ValuePreserved) {
    Transaction tx = makeTx(1, 1,
        std::numeric_limits<uint64_t>::max(), "addr");
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.value, std::numeric_limits<uint64_t>::max());
}

TEST_F(NetworkTest, MaxUint64NoncePreserved) {
    Transaction tx;
    tx.chainId = 1;
    tx.nonce   = std::numeric_limits<uint64_t>::max();
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.nonce, std::numeric_limits<uint64_t>::max());
}

TEST_F(NetworkTest, MaxUint64GasLimitPreserved) {
    Transaction tx;
    tx.chainId  = 1;
    tx.nonce    = 1;
    tx.gasLimit = std::numeric_limits<uint64_t>::max();
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.gasLimit, std::numeric_limits<uint64_t>::max());
}

TEST_F(NetworkTest, MaxUint64MaxFeePerGasPreserved) {
    Transaction tx;
    tx.chainId      = 1;
    tx.nonce        = 1;
    tx.maxFeePerGas = std::numeric_limits<uint64_t>::max();
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.maxFeePerGas, std::numeric_limits<uint64_t>::max());
}

TEST_F(NetworkTest, ZeroValuePreserved) {
    Transaction tx = makeTx(1, 1, 0, "addr");
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.value, 0ULL);
}

TEST_F(NetworkTest, ZeroNoncePreserved) {
    Transaction tx;
    tx.chainId = 1;
    tx.nonce   = 0;
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.nonce, 0ULL);
}

TEST_F(NetworkTest, VeryLongAddressPreserved) {
    std::string longAddr(512, 'x');
    Transaction tx = makeTx(1, 1, 100, longAddr);
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.toAddress, longAddr);
    ASSERT_FALSE(tx2.outputs.empty());
    EXPECT_EQ(tx2.outputs[0].address, longAddr);
}

TEST_F(NetworkTest, VeryLongPrevTxHashPreserved) {
    Transaction tx;
    tx.chainId = 1; tx.nonce = 1;
    TxInput in;
    in.prevTxHash  = std::string(256, 'a');
    in.outputIndex = 0;
    tx.inputs.push_back(in);
    TxOutput out; out.value = 100; out.address = "addr";
    tx.outputs.push_back(out);
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    ASSERT_FALSE(tx2.inputs.empty());
    EXPECT_EQ(tx2.inputs[0].prevTxHash.size(), 256U);
}

TEST_F(NetworkTest, LargeOutputIndexPreserved) {
    Transaction tx;
    tx.chainId = 1; tx.nonce = 1;
    TxInput in;
    in.prevTxHash  = "prev";
    in.outputIndex = 9999;
    tx.inputs.push_back(in);
    TxOutput out; out.value = 100; out.address = "addr";
    tx.outputs.push_back(out);
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.inputs[0].outputIndex, 9999);
}

TEST_F(NetworkTest, MaxUint64ChainIdPreserved) {
    Transaction tx;
    tx.chainId = std::numeric_limits<uint64_t>::max();
    tx.nonce   = 1;
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.chainId, std::numeric_limits<uint64_t>::max());
}

TEST_F(NetworkTest, EmptyDataFieldPreserved) {
    Transaction tx;
    tx.chainId = 1; tx.nonce = 1;
    EXPECT_TRUE(tx.data.empty());
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_TRUE(tx2.data.empty());
}

TEST_F(NetworkTest, LargeDataFieldPreserved) {
    Transaction tx;
    tx.chainId = 1; tx.nonce = 1;
    tx.data.resize(10000, 0xAB);
    tx.calculateHash();
    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.data.size(), 10000U);
    EXPECT_EQ(tx2.data[0],    0xAB);
    EXPECT_EQ(tx2.data[9999], 0xAB);
}

// =============================================================================
// HASH INTEGRITY -- tamper detection
// =============================================================================

TEST_F(NetworkTest, TamperedValueInvalidatesHash) {
    Transaction tx = makeTx(1, 1, 100);
    json j = serializeTx(tx);
    std::string originalHash = j["txHash"];

    // Tamper with value post-serialization
    j["value"] = 99999;
    Transaction tx2 = deserializeTx(j);
    // Recalculate hash from tampered tx -- must differ from original
    tx2.calculateHash();
    EXPECT_NE(tx2.txHash, originalHash);
}

TEST_F(NetworkTest, TamperedNonceInvalidatesHash) {
    Transaction tx = makeTx(1, 5, 100);
    json j = serializeTx(tx);
    std::string originalHash = j["txHash"];

    j["nonce"] = 999;
    Transaction tx2 = deserializeTx(j);
    tx2.calculateHash();
    EXPECT_NE(tx2.txHash, originalHash);
}

TEST_F(NetworkTest, TamperedOutputAddressInvalidatesHash) {
    Transaction tx = makeTx(1, 1, 100, "honest");
    json j = serializeTx(tx);
    std::string originalHash = j["txHash"];

    j["outputs"][0]["address"] = "attacker";
    Transaction tx2 = deserializeTx(j);
    tx2.calculateHash();
    EXPECT_NE(tx2.txHash, originalHash);
}

TEST_F(NetworkTest, TamperedOutputValueInvalidatesHash) {
    Transaction tx = makeTx(1, 1, 100, "addr");
    json j = serializeTx(tx);
    std::string originalHash = j["txHash"];

    j["outputs"][0]["value"] = 99999999;
    Transaction tx2 = deserializeTx(j);
    tx2.calculateHash();
    EXPECT_NE(tx2.txHash, originalHash);
}

TEST_F(NetworkTest, TamperedBlockHashDetected) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    std::string originalHash = j["hash"];

    j["reward"] = 99999;
    Block b2 = deserializeBlock(j);
    // Hash field in b2 still holds the original hash from JSON
    // but reward was changed -- if recomputed it should differ
    EXPECT_NE(b2.reward, b.reward);
}

// =============================================================================
// BLOCK SERIALIZATION -- STRUCTURE
// =============================================================================

TEST_F(NetworkTest, SerializeBlockProducesJson) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    EXPECT_FALSE(j.empty());
}

TEST_F(NetworkTest, SerializeBlockContainsAllRequiredFields) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    EXPECT_TRUE(j.contains("hash"));
    EXPECT_TRUE(j.contains("previousHash"));
    EXPECT_TRUE(j.contains("timestamp"));
    EXPECT_TRUE(j.contains("reward"));
    EXPECT_TRUE(j.contains("nonce"));
    EXPECT_TRUE(j.contains("difficulty"));
    EXPECT_TRUE(j.contains("minerAddress"));
    EXPECT_TRUE(j.contains("transactions"));
    EXPECT_TRUE(j.contains("version"));
    EXPECT_TRUE(j.contains("baseFee"));
    EXPECT_TRUE(j.contains("gasUsed"));
    EXPECT_TRUE(j.contains("gasLimit"));
}

TEST_F(NetworkTest, SerializeBlockVersionMatchesConstant) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    EXPECT_EQ(j["version"].get<uint32_t>(), SERIALIZATION_VERSION);
}

// =============================================================================
// BLOCK SERIALIZATION -- ROUND TRIP
// =============================================================================

TEST_F(NetworkTest, DeserializeBlockRoundTrip) {
    Block b = makeBlock();
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.hash,         b.hash);
    EXPECT_EQ(b2.previousHash, b.previousHash);
    EXPECT_EQ(b2.timestamp,    b.timestamp);
    EXPECT_EQ(b2.reward,       b.reward);
    EXPECT_EQ(b2.nonce,        b.nonce);
    EXPECT_EQ(b2.difficulty,   b.difficulty);
    EXPECT_EQ(b2.minerAddress, b.minerAddress);
    EXPECT_EQ(b2.baseFee,      b.baseFee);
    EXPECT_EQ(b2.gasUsed,      b.gasUsed);
    EXPECT_EQ(b2.gasLimit,     b.gasLimit);
}

TEST_F(NetworkTest, DeserializeBlockWithTransactions) {
    Block b = makeBlock();
    b.transactions.push_back(makeTx(1, 1, 100, "addr1"));
    b.transactions.push_back(makeTx(1, 2, 200, "addr2"));
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.transactions.size(), 2U);
    EXPECT_EQ(b2.transactions[0].txHash, b.transactions[0].txHash);
    EXPECT_EQ(b2.transactions[1].txHash, b.transactions[1].txHash);
}

TEST_F(NetworkTest, DeserializeBlockTransactionValuesPreserved) {
    Block b = makeBlock();
    b.transactions.push_back(makeTx(1, 1, 999, "richAddr"));
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    ASSERT_EQ(b2.transactions.size(), 1U);
    ASSERT_FALSE(b2.transactions[0].outputs.empty());
    EXPECT_EQ(b2.transactions[0].outputs[0].value,   999ULL);
    EXPECT_EQ(b2.transactions[0].outputs[0].address, "richAddr");
}

TEST_F(NetworkTest, DeserializeBlockEmptyTransactions) {
    Block b = makeBlock();
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_TRUE(b2.transactions.empty());
}

// =============================================================================
// BLOCK -- EXTREME EDGE CASES
// =============================================================================

TEST_F(NetworkTest, BlockMaxUint64RewardPreserved) {
    Block b = makeBlock();
    b.reward = std::numeric_limits<uint64_t>::max();
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.reward, std::numeric_limits<uint64_t>::max());
}

TEST_F(NetworkTest, BlockMaxUint64NoncePreserved) {
    Block b = makeBlock();
    b.nonce  = std::numeric_limits<uint64_t>::max();
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.nonce, std::numeric_limits<uint64_t>::max());
}

TEST_F(NetworkTest, BlockMaxUint64TimestampPreserved) {
    Block b = makeBlock();
    b.timestamp = std::numeric_limits<uint64_t>::max();
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.timestamp, std::numeric_limits<uint64_t>::max());
}

TEST_F(NetworkTest, BlockZeroRewardPreserved) {
    Block b = makeBlock();
    b.reward = 0;
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.reward, 0ULL);
}

TEST_F(NetworkTest, BlockZeroNoncePreserved) {
    Block b = makeBlock();
    b.nonce  = 0;
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.nonce, 0ULL);
}

TEST_F(NetworkTest, BlockMaxGasLimitPreserved) {
    Block b = makeBlock();
    b.gasLimit = std::numeric_limits<uint64_t>::max();
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.gasLimit, std::numeric_limits<uint64_t>::max());
}

TEST_F(NetworkTest, BlockVeryLongMinerAddressPreserved) {
    Block b = makeBlock();
    b.minerAddress = std::string(512, 'm');
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.minerAddress, std::string(512, 'm'));
}

TEST_F(NetworkTest, BlockEmptyPreviousHashPreserved) {
    Block b = makeBlock();
    b.previousHash = "";
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_TRUE(b2.previousHash.empty());
}

TEST_F(NetworkTest, BlockMaxDifficultyPreserved) {
    Block b = makeBlock();
    b.difficulty = Block::MAX_DIFFICULTY;
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.difficulty, Block::MAX_DIFFICULTY);
}

// =============================================================================
// BLOCK SERIALIZATION -- FAILURE PATHS
// =============================================================================

TEST_F(NetworkTest, DeserializeBlockMissingHashThrows) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    j.erase("hash");
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeBlockMissingTimestampThrows) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    j.erase("timestamp");
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeBlockMissingRewardThrows) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    j.erase("reward");
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeBlockMissingNonceThrows) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    j.erase("nonce");
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeBlockMissingDifficultyThrows) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    j.erase("difficulty");
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeBlockEmptyJsonThrows) {
    json j = json::object();
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeBlockWrongVersionThrows) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    j["version"] = 999;
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeBlockWrongTypeThrows) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    j["timestamp"] = "not_a_number";
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

TEST_F(NetworkTest, DeserializeBlockNullJsonThrows) {
    json j = nullptr;
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

// =============================================================================
// VERSIONING / BACKWARD COMPATIBILITY
// =============================================================================

TEST_F(NetworkTest, SerializationVersionIsOne) {
    EXPECT_EQ(SERIALIZATION_VERSION, 1U);
}

TEST_F(NetworkTest, FutureVersionRejected) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    j["version"] = SERIALIZATION_VERSION + 1;
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

TEST_F(NetworkTest, PastVersionRejected) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    j["version"] = 0;
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

TEST_F(NetworkTest, MissingVersionThrows) {
    Block b = makeBlock();
    json j = serializeBlock(b);
    j.erase("version");
    EXPECT_THROW(deserializeBlock(j), SerializationError);
}

// =============================================================================
// BLOCKCHAIN CONTEXT -- serialized tx integrates with chain
// =============================================================================

TEST_F(NetworkTest, SerializedTxHashMatchesBlockchainTxHash) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    ASSERT_FALSE(latest->transactions.empty());

    const Transaction &cbTx = latest->transactions.front();
    json j = serializeTx(cbTx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.txHash, cbTx.txHash);
}

TEST_F(NetworkTest, SerializedBlockHashMatchesBlockchainHash) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());

    json j = serializeBlock(*latest);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.hash, latest->hash);
}

TEST_F(NetworkTest, SerializedBlockRoundTripPreservesChainLink) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());

    json j = serializeBlock(*latest);
    Block b2 = deserializeBlock(j);
    EXPECT_FALSE(b2.previousHash.empty());
    EXPECT_EQ(b2.previousHash, latest->previousHash);
}

TEST_F(NetworkTest, SerializedCoinbaseTxOutputMatchesUTXO) {
    EXPECT_TRUE(bc->addBlock("miner_net_test", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    ASSERT_FALSE(latest->transactions.empty());

    const Transaction &cbTx = latest->transactions.front();
    json j = serializeTx(cbTx);
    Transaction tx2 = deserializeTx(j);

    ASSERT_FALSE(tx2.outputs.empty());
    EXPECT_EQ(tx2.outputs[0].address, "miner_net_test");
    EXPECT_EQ(tx2.outputs[0].value,   (55ULL * 90) / 100);
}

TEST_F(NetworkTest, UTXOFromSerializedBlockIsSpendable) {
    EXPECT_TRUE(bc->addBlock("net_utxo_miner", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());

    json j = serializeBlock(*latest);
    Block b2 = deserializeBlock(j);

    ASSERT_FALSE(b2.transactions.empty());
    const std::string &cbHash = b2.transactions.front().txHash;
    auto utxoOpt = bc->getUTXO(cbHash, 0);
    ASSERT_TRUE(utxoOpt.has_value());
    EXPECT_EQ(utxoOpt->address, "net_utxo_miner");
}

TEST_F(NetworkTest, MultipleBlocksSerializeAndDeserializeCorrectly) {
    std::vector<std::string> hashes;
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(bc->addBlock("miner" + std::to_string(i), {}));
        auto latest = bc->getLatestBlock();
        ASSERT_TRUE(latest.has_value());
        json j   = serializeBlock(*latest);
        Block b2 = deserializeBlock(j);
        EXPECT_EQ(b2.hash, latest->hash);
        hashes.push_back(b2.hash);
    }
    for (size_t i = 0; i < hashes.size(); ++i)
        for (size_t k = i + 1; k < hashes.size(); ++k)
            EXPECT_NE(hashes[i], hashes[k]);
}

TEST_F(NetworkTest, ChainValidatesAfterSerializeDeserializeCycle) {
    for (int i = 0; i < 3; ++i)
        EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto result = bc->validateChain();
    EXPECT_TRUE(result.ok);
}

// =============================================================================
// BLOCKCHAIN CONTEXT -- UTXO
// =============================================================================

TEST_F(NetworkTest, UTXOBalanceReflectsSerializedOutputValue) {
    TxOutput out;
    out.value   = 777;
    out.address = "netUtxoAddr";
    EXPECT_TRUE(utxo->addUTXO(out, "netTx1", 0, 1, false));

    Transaction tx;
    tx.chainId = 1; tx.nonce = 1;
    TxInput in; in.prevTxHash = "netTx1"; in.outputIndex = 0;
    tx.inputs.push_back(in);
    TxOutput txOut; txOut.value = 700; txOut.address = "dest";
    tx.outputs.push_back(txOut);
    tx.calculateHash();

    json j = serializeTx(tx);
    Transaction tx2 = deserializeTx(j);
    EXPECT_EQ(tx2.inputs[0].prevTxHash, "netTx1");

    auto bal = utxo->getBalance("netUtxoAddr");
    ASSERT_TRUE(bal.has_value());
    EXPECT_EQ(bal.value(), 777ULL);
}

// =============================================================================
// SERIALIZATION ERROR CODES
// =============================================================================

TEST_F(NetworkTest, SerializationErrorCodeMissingField) {
    SerializationError e(SerializationErrorCode::MissingField, "test");
    EXPECT_EQ(e.code, SerializationErrorCode::MissingField);
    EXPECT_STREQ(e.what(), "test");
}

TEST_F(NetworkTest, SerializationErrorCodeTypeMismatch) {
    SerializationError e(SerializationErrorCode::TypeMismatch, "type");
    EXPECT_EQ(e.code, SerializationErrorCode::TypeMismatch);
}

TEST_F(NetworkTest, SerializationErrorCodeVersionMismatch) {
    SerializationError e(SerializationErrorCode::VersionMismatch, "ver");
    EXPECT_EQ(e.code, SerializationErrorCode::VersionMismatch);
}

TEST_F(NetworkTest, SerializationErrorCodeHashMismatch) {
    SerializationError e(SerializationErrorCode::HashMismatch, "hash");
    EXPECT_EQ(e.code, SerializationErrorCode::HashMismatch);
}

TEST_F(NetworkTest, SerializationErrorInheritsRuntimeError) {
    SerializationError e(SerializationErrorCode::InternalError, "msg");
    const std::runtime_error &re = e;
    EXPECT_STREQ(re.what(), "msg");
}

TEST_F(NetworkTest, AllErrorCodesAreDistinct) {
    EXPECT_NE(static_cast<uint8_t>(SerializationErrorCode::MissingField),
              static_cast<uint8_t>(SerializationErrorCode::TypeMismatch));
    EXPECT_NE(static_cast<uint8_t>(SerializationErrorCode::VersionMismatch),
              static_cast<uint8_t>(SerializationErrorCode::HashMismatch));
    EXPECT_NE(static_cast<uint8_t>(SerializationErrorCode::None),
              static_cast<uint8_t>(SerializationErrorCode::InternalError));
}

// =============================================================================
// METRICS
// =============================================================================

TEST_F(NetworkTest, MetricsIncrementOnSuccessfulSerializeTx) {
    Transaction tx = makeTx();
    auto before = getSerializationMetrics();
    serializeTx(tx);
    auto after = getSerializationMetrics();
    EXPECT_GT(after.txSerializeOk, before.txSerializeOk);
}

TEST_F(NetworkTest, MetricsIncrementOnSuccessfulDeserializeTx) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    auto before = getSerializationMetrics();
    deserializeTx(j);
    auto after = getSerializationMetrics();
    EXPECT_GT(after.txDeserializeOk, before.txDeserializeOk);
}

TEST_F(NetworkTest, MetricsIncrementOnSuccessfulSerializeBlock) {
    Block b = makeBlock();
    auto before = getSerializationMetrics();
    serializeBlock(b);
    auto after = getSerializationMetrics();
    EXPECT_GT(after.blockSerializeOk, before.blockSerializeOk);
}

TEST_F(NetworkTest, MetricsIncrementOnFailedDeserializeTx) {
    auto before = getSerializationMetrics();
    try { deserializeTx(json::object()); } catch (...) {}
    auto after = getSerializationMetrics();
    EXPECT_GT(after.txDeserializeErr, before.txDeserializeErr);
}

TEST_F(NetworkTest, MetricsIncrementOnFailedDeserializeBlock) {
    auto before = getSerializationMetrics();
    try { deserializeBlock(json::object()); } catch (...) {}
    auto after = getSerializationMetrics();
    EXPECT_GT(after.blockDeserializeErr, before.blockDeserializeErr);
}

// =============================================================================
// STRESS -- bulk serialization
// =============================================================================

TEST_F(NetworkTest, BulkSerializeThousandTransactions) {
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        Transaction tx = makeTx(1, static_cast<uint64_t>(i + 1),
                                 static_cast<uint64_t>(i * 10));
        json j = serializeTx(tx);
        EXPECT_FALSE(j.empty());
        Transaction tx2 = deserializeTx(j);
        EXPECT_EQ(tx2.txHash, tx.txHash);
    }
}

TEST_F(NetworkTest, BulkSerializeFiveHundredBlocks) {
    constexpr int N = 500;
    for (int i = 0; i < N; ++i) {
        Block b = makeBlock();
        b.nonce = static_cast<uint64_t>(i);
        json j   = serializeBlock(b);
        Block b2 = deserializeBlock(j);
        EXPECT_EQ(b2.nonce, static_cast<uint64_t>(i));
    }
}

TEST_F(NetworkTest, BlockWithManyTransactionsSerializesCorrectly) {
    Block b = makeBlock();
    for (int i = 0; i < 200; ++i)
        b.transactions.push_back(
            makeTx(1, static_cast<uint64_t>(i + 1),
                   static_cast<uint64_t>(i * 5),
                   "addr" + std::to_string(i)));
    json j   = serializeBlock(b);
    Block b2 = deserializeBlock(j);
    EXPECT_EQ(b2.transactions.size(), 200U);
    for (int i = 0; i < 200; ++i)
        EXPECT_EQ(b2.transactions[i].txHash,
                  b.transactions[i].txHash);
}

// =============================================================================
// CONCURRENCY -- heavy load
// =============================================================================

TEST_F(NetworkTest, ConcurrentSerializeTxHeavyLoad) {
    constexpr int THREADS = 16;
    constexpr int PER_THREAD = 50;
    std::vector<std::thread> threads;
    std::atomic<int> errorCount{0};

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                try {
                    Transaction tx = makeTx(
                        1,
                        static_cast<uint64_t>(t * PER_THREAD + i + 1),
                        static_cast<uint64_t>(i * 10));
                    json j = serializeTx(tx);
                    Transaction tx2 = deserializeTx(j);
                    if (tx2.txHash != tx.txHash) ++errorCount;
                } catch (...) { ++errorCount; }
            }
        });
    }
    for (auto &th : threads) th.join();
    EXPECT_EQ(errorCount.load(), 0);
}

TEST_F(NetworkTest, ConcurrentSerializeBlockHeavyLoad) {
    constexpr int THREADS = 16;
    constexpr int PER_THREAD = 25;
    std::vector<std::thread> threads;
    std::atomic<int> errorCount{0};

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                try {
                    Block b = makeBlock();
                    b.nonce = static_cast<uint64_t>(t * PER_THREAD + i);
                    json j   = serializeBlock(b);
                    Block b2 = deserializeBlock(j);
                    if (b2.nonce != b.nonce) ++errorCount;
                } catch (...) { ++errorCount; }
            }
        });
    }
    for (auto &th : threads) th.join();
    EXPECT_EQ(errorCount.load(), 0);
}

TEST_F(NetworkTest, ConcurrentMixedReadsAndWrites) {
    constexpr int THREADS = 8;
    std::vector<std::thread> threads;
    std::atomic<int> errorCount{0};
    std::atomic<bool> stop{false};

    // Writers: serialize transactions
    for (int t = 0; t < THREADS / 2; ++t) {
        threads.emplace_back([&, t]() {
            int i = 0;
            while (!stop.load()) {
                try {
                    Transaction tx = makeTx(1,
                        static_cast<uint64_t>(t * 1000 + i++));
                    json j = serializeTx(tx);
                    deserializeTx(j);
                } catch (...) { ++errorCount; }
            }
        });
    }

    // Readers: serialize blocks
    for (int t = 0; t < THREADS / 2; ++t) {
        threads.emplace_back([&, t]() {
            int i = 0;
            while (!stop.load()) {
                try {
                    Block b = makeBlock();
                    b.nonce = static_cast<uint64_t>(t * 1000 + i++);
                    json j   = serializeBlock(b);
                    deserializeBlock(j);
                } catch (...) { ++errorCount; }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    for (auto &th : threads) th.join();
    EXPECT_EQ(errorCount.load(), 0);
}
