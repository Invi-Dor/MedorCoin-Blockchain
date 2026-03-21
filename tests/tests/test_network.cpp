#include <gtest/gtest.h>
#include "net/serialization.h"
#include "transaction.h"
#include "block.h"
#include "blockchain.h"
#include "utxo.h"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
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

// Returns true if the slow performance tests should be skipped.
// Set environment variable MEDOR_SKIP_SLOW_TESTS=1 when running
// locally to skip them. On CI leave the variable unset so every
// test runs.
static bool skipSlowTests()
{
    const char *env = std::getenv("MEDOR_SKIP_SLOW_TESTS");
    return env != nullptr && std::string(env) == "1";
}

// =============================================================================
// FIXTURE
// =============================================================================

class NetworkTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        static std::atomic<int> counter{0};
        suffix_ = std::to_string(++counter);
        bc   = std::make_unique<Blockchain>(netTestConfig(suffix_));
        utxo = std::make_unique<UTXOSet>();
    }

    void TearDown() override
    {
        bc.reset();
        utxo.reset();
        std::filesystem::remove_all(
            "/tmp/test_net_blockdb"   + suffix_);
        std::filesystem::remove_all(
            "/tmp/test_net_accountdb" + suffix_);
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
    EXPECT_EQ(j["nonce"].get<uint64_t>(), 99ULL);
}

TEST_F(NetworkTest, SerializeTxContainsValue) {
    Transaction tx = makeTx(1, 1, 777);
    json j = serializeTx(tx);
    EXPECT_EQ(j["value"].get<uint64_t>(), 777ULL);
}

TEST_F(NetworkTest, SerializeTxContainsInputs) {
    Transaction tx = makeTx();
    json j = serializeTx(tx);
    EXPECT_TRUE(j.contains("inputs"));
    EXPECT_TRUE(j["inputs"].is_array());
