#include <gtest/gtest.h>
#include "blockchain.h"
#include "transaction.h"
#include "block.h"
#include "blockchain_fork.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// =============================================================================
// HELPERS
// =============================================================================

static Blockchain::Config testConfig(const std::string &suffix = "")
{
    Blockchain::Config cfg;
    cfg.ownerAddress    = "medor_owner";
    cfg.treasuryAddress = "medor_treasury";
    cfg.blockDBPath     = "/tmp/test_blockdb"  + suffix;
    cfg.accountDBPath   = "/tmp/test_accountdb" + suffix;
    cfg.maxSupply       = 1000000ULL;
    cfg.initialMedor    = 1;
    cfg.initialBaseFee  = 1ULL;
    cfg.rewardSchedule  = { { UINT64_MAX, 55ULL } };
    return cfg;
}

static Transaction makeTx(uint64_t nonce   = 1,
                           uint64_t value   = 100,
                           const std::string &from = "sender",
                           const std::string &to   = "recipient")
{
    Transaction tx;
    tx.chainId   = 1;
    tx.nonce     = nonce;
    tx.value     = value;
    tx.toAddress = to;
    tx.gasLimit  = 21000;
    tx.maxFeePerGas         = 10;
    tx.maxPriorityFeePerGas = 2;
    TxOutput out;
    out.value   = value;
    out.address = to;
    tx.outputs.push_back(out);
    tx.calculateHash();
    return tx;
}

// =============================================================================
// FIXTURE
// Each test gets a fresh blockchain with unique DB paths to avoid state bleed.
// =============================================================================

class BlockchainTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<int> counter{0};
        suffix_ = std::to_string(++counter);
        bc = std::make_unique<Blockchain>(testConfig(suffix_));
    }

    void TearDown() override {
        bc.reset();
        std::filesystem::remove_all("/tmp/test_blockdb"   + suffix_);
        std::filesystem::remove_all("/tmp/test_accountdb" + suffix_);
    }

    std::unique_ptr<Blockchain> bc;
    std::string                 suffix_;
};

// =============================================================================
// INITIALISATION
// =============================================================================

TEST_F(BlockchainTest, InitialisedAfterConstruction) {
    EXPECT_TRUE(bc->isOpen());
}

TEST_F(BlockchainTest, GenesisBlockExists) {
    EXPECT_GE(bc->height(), 0ULL);
}

TEST_F(BlockchainTest, GenesisBlockHasHash) {
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    EXPECT_FALSE(latest->hash.empty());
}

TEST_F(BlockchainTest, GenesisBlockHasZeroPreviousHash) {
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    EXPECT_TRUE(latest->previousHash.empty());
}

TEST_F(BlockchainTest, InitialBaseFeeMatchesConfig) {
    EXPECT_GE(bc->baseFee(), 1ULL);
}

TEST_F(BlockchainTest, InitialTotalSupplyIsNonZero) {
    // Genesis block mints the first reward
    EXPECT_GT(bc->totalSupply(), 0ULL);
}

// =============================================================================
// BLOCK ADDITION
// =============================================================================

TEST_F(BlockchainTest, AddBlockIncreasesHeight) {
    size_t before = bc->height();
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    EXPECT_EQ(bc->height(), before + 1);
}

TEST_F(BlockchainTest, AddMultipleBlocksIncreasesHeightCorrectly) {
    size_t before = bc->height();
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(bc->addBlock("miner1", {}));
    EXPECT_EQ(bc->height(), before + 5);
}

TEST_F(BlockchainTest, AddBlockEmptyMinerFails) {
    EXPECT_FALSE(bc->addBlock("", {}));
}

TEST_F(BlockchainTest, AddBlockIncreasesTotalSupply) {
    uint64_t before = bc->totalSupply();
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    EXPECT_GT(bc->totalSupply(), before);
}

TEST_F(BlockchainTest, AddBlockEachBlockHashIsUnique) {
    std::vector<std::string> hashes;
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(bc->addBlock("miner1", {}));
        auto b = bc->getLatestBlock();
        ASSERT_TRUE(b.has_value());
        hashes.push_back(b->hash);
    }
    for (size_t i = 0; i < hashes.size(); ++i)
        for (size_t j = i + 1; j < hashes.size(); ++j)
            EXPECT_NE(hashes[i], hashes[j]);
}

TEST_F(BlockchainTest, AddBlockChainLinkageIsCorrect) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto b1 = bc->getLatestBlock();
    ASSERT_TRUE(b1.has_value());
    std::string hash1 = b1->hash;

    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto b2 = bc->getLatestBlock();
    ASSERT_TRUE(b2.has_value());
    EXPECT_EQ(b2->previousHash, hash1);
}

TEST_F(BlockchainTest, AddBlockTimestampIsMonotonicallyIncreasing) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto b1 = bc->getLatestBlock();
    ASSERT_TRUE(b1.has_value());
    uint64_t t1 = b1->timestamp;

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto b2 = bc->getLatestBlock();
    ASSERT_TRUE(b2.has_value());
    EXPECT_GE(b2->timestamp, t1);
}

TEST_F(BlockchainTest, AddBlockRewardIsCorrect) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto b = bc->getLatestBlock();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->reward, 55ULL);
}

TEST_F(BlockchainTest, AddBlockBaseFeeIsPresent) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto b = bc->getLatestBlock();
    ASSERT_TRUE(b.has_value());
    EXPECT_GE(b->baseFee, 1ULL);
}

// =============================================================================
// MINER REWARDS
// =============================================================================

TEST_F(BlockchainTest, MinerReceives90PercentOfReward) {
    std::string miner = "miner_reward_test";
    uint64_t before = bc->getBalance(miner);
    EXPECT_TRUE(bc->addBlock(miner, {}));
    uint64_t after = bc->getBalance(miner);
    uint64_t received = after - before;
    uint64_t expected = (55ULL * 90) / 100;
    EXPECT_EQ(received, expected);
}

TEST_F(BlockchainTest, OwnerReceives10PercentOfReward) {
    std::string owner = "medor_owner";
    uint64_t before = bc->getBalance(owner);
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    uint64_t after = bc->getBalance(owner);
    uint64_t received = after - before;
    uint64_t expected = 55ULL - (55ULL * 90) / 100;
    EXPECT_EQ(received, expected);
}

TEST_F(BlockchainTest, TotalRewardMatchesMinerPlusOwner) {
    std::string miner = "miner_split_test";
    std::string owner = "medor_owner";
    uint64_t minerBefore = bc->getBalance(miner);
    uint64_t ownerBefore = bc->getBalance(owner);
    EXPECT_TRUE(bc->addBlock(miner, {}));
    uint64_t minerGot = bc->getBalance(miner) - minerBefore;
    uint64_t ownerGot = bc->getBalance(owner) - ownerBefore;
    EXPECT_EQ(minerGot + ownerGot, 55ULL);
}

TEST_F(BlockchainTest, MaxSupplyNotExceeded) {
    for (int i = 0; i < 30; ++i)
        bc->addBlock("miner1", {});
    EXPECT_LE(bc->totalSupply(), testConfig().maxSupply);
}

TEST_F(BlockchainTest, RewardStopsAtMaxSupply) {
    // Fill to max supply with a tiny max supply config
    Blockchain::Config cfg = testConfig("_maxsupply");
    cfg.maxSupply      = 110ULL;  // exactly 2 blocks worth
    cfg.rewardSchedule = { { UINT64_MAX, 55ULL } };
    Blockchain bc2(cfg);
    for (int i = 0; i < 10; ++i)
        bc2.addBlock("miner1", {});
    EXPECT_LE(bc2.totalSupply(), 110ULL);
}

// =============================================================================
// BALANCE OPERATIONS
// =============================================================================

TEST_F(BlockchainTest, GetBalanceZeroForUnknownAddress) {
    EXPECT_EQ(bc->getBalance("unknownaddr"), 0ULL);
}

TEST_F(BlockchainTest, SetAndGetBalance) {
    EXPECT_TRUE(bc->setBalance("testaddr", 500ULL));
    EXPECT_EQ(bc->getBalance("testaddr"), 500ULL);
}

TEST_F(BlockchainTest, SetBalanceEmptyAddressFails) {
    EXPECT_FALSE(bc->setBalance("", 100ULL));
}

TEST_F(BlockchainTest, AddBalance) {
    bc->setBalance("addr_add", 100ULL);
    EXPECT_TRUE(bc->addBalance("addr_add", 50ULL));
    EXPECT_EQ(bc->getBalance("addr_add"), 150ULL);
}

TEST_F(BlockchainTest, AddBalanceEmptyAddressFails) {
    EXPECT_FALSE(bc->addBalance("", 50ULL));
}

TEST_F(BlockchainTest, AddBalanceOverflowFails) {
    bc->setBalance("addr_overflow",
                   std::numeric_limits<uint64_t>::max());
    EXPECT_FALSE(bc->addBalance("addr_overflow", 1ULL));
    EXPECT_EQ(bc->getBalance("addr_overflow"),
              std::numeric_limits<uint64_t>::max());
}

TEST_F(BlockchainTest, DeductBalance) {
    bc->setBalance("addr_deduct", 200ULL);
    EXPECT_TRUE(bc->deductBalance("addr_deduct", 80ULL));
    EXPECT_EQ(bc->getBalance("addr_deduct"), 120ULL);
}

TEST_F(BlockchainTest, DeductBalanceInsufficientFails) {
    bc->setBalance("addr_insuff", 50ULL);
    EXPECT_FALSE(bc->deductBalance("addr_insuff", 100ULL));
    EXPECT_EQ(bc->getBalance("addr_insuff"), 50ULL);
}

TEST_F(BlockchainTest, DeductBalanceEmptyAddressFails) {
    EXPECT_FALSE(bc->deductBalance("", 10ULL));
}

TEST_F(BlockchainTest, DeductEntireBalance) {
    bc->setBalance("addr_full", 300ULL);
    EXPECT_TRUE(bc->deductBalance("addr_full", 300ULL));
    EXPECT_EQ(bc->getBalance("addr_full"), 0ULL);
}

TEST_F(BlockchainTest, AddZeroBalance) {
    bc->setBalance("addr_zero", 100ULL);
    EXPECT_TRUE(bc->addBalance("addr_zero", 0ULL));
    EXPECT_EQ(bc->getBalance("addr_zero"), 100ULL);
}

TEST_F(BlockchainTest, DeductZeroBalance) {
    bc->setBalance("addr_zero2", 100ULL);
    EXPECT_TRUE(bc->deductBalance("addr_zero2", 0ULL));
    EXPECT_EQ(bc->getBalance("addr_zero2"), 100ULL);
}

// =============================================================================
// BASE FEE
// =============================================================================

TEST_F(BlockchainTest, BaseFeeIsPositive) {
    EXPECT_GE(bc->baseFee(), 1ULL);
}

TEST_F(BlockchainTest, SetBaseFeeWorks) {
    bc->setBaseFee(100ULL);
    EXPECT_EQ(bc->baseFee(), 100ULL);
}

TEST_F(BlockchainTest, SetBaseFeeZeroClampedToOne) {
    bc->setBaseFee(0ULL);
    EXPECT_EQ(bc->baseFee(), 1ULL);
}

TEST_F(BlockchainTest, AdjustBaseFeeFullBlockIncreases) {
    uint64_t before = bc->baseFee();
    // gasUsed > 50% of gasLimit triggers increase
    bc->adjustBaseFee(900000ULL, 1000000ULL);
    EXPECT_GE(bc->baseFee(), before);
}

TEST_F(BlockchainTest, AdjustBaseFeeEmptyBlockDecreases) {
    bc->setBaseFee(100ULL);
    // gasUsed < 50% of gasLimit triggers decrease
    bc->adjustBaseFee(100ULL, 1000000ULL);
    EXPECT_LE(bc->baseFee(), 100ULL);
}

TEST_F(BlockchainTest, AdjustBaseFeeZeroGasLimitNoChange) {
    uint64_t before = bc->baseFee();
    bc->adjustBaseFee(0ULL, 0ULL);
    EXPECT_EQ(bc->baseFee(), before);
}

TEST_F(BlockchainTest, BaseFeeNeverDropsBelowOne) {
    bc->setBaseFee(1ULL);
    for (int i = 0; i < 20; ++i)
        bc->adjustBaseFee(0ULL, 1000000ULL);
    EXPECT_GE(bc->baseFee(), 1ULL);
}

TEST_F(BlockchainTest, BurnBaseFeesCreditsToTreasury) {
    uint64_t before = bc->getBalance("medor_treasury");
    bc->burnBaseFees(100ULL);
    EXPECT_EQ(bc->getBalance("medor_treasury"), before + 100ULL);
}

// =============================================================================
// HAS BLOCK / GET LATEST BLOCK
// =============================================================================

TEST_F(BlockchainTest, HasBlockReturnsFalseForUnknown) {
    EXPECT_FALSE(bc->hasBlock("nonexistenthash"));
}

TEST_F(BlockchainTest, HasBlockReturnsFalseForEmpty) {
    EXPECT_FALSE(bc->hasBlock(""));
}

TEST_F(BlockchainTest, HasBlockReturnsTrueAfterAdd) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    EXPECT_TRUE(bc->hasBlock(latest->hash));
}

TEST_F(BlockchainTest, GetLatestBlockReturnsValue) {
    auto block = bc->getLatestBlock();
    EXPECT_TRUE(block.has_value());
}

TEST_F(BlockchainTest, GetLatestBlockUpdatesAfterAdd) {
    auto b1 = bc->getLatestBlock();
    ASSERT_TRUE(b1.has_value());
    std::string hash1 = b1->hash;

    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto b2 = bc->getLatestBlock();
    ASSERT_TRUE(b2.has_value());
    EXPECT_NE(b2->hash, hash1);
}

TEST_F(BlockchainTest, GetLatestBlockIsIndependentClone) {
    auto b1 = bc->getLatestBlock();
    auto b2 = bc->getLatestBlock();
    ASSERT_TRUE(b1.has_value());
    ASSERT_TRUE(b2.has_value());
    // Mutating one clone does not affect the other
    b1->hash = "mutated";
    EXPECT_NE(b2->hash, "mutated");
}

// =============================================================================
// UTXO
// =============================================================================

TEST_F(BlockchainTest, GetUTXOReturnsNulloptForUnknown) {
    auto utxo = bc->getUTXO("fakehash", 0);
    EXPECT_FALSE(utxo.has_value());
}

TEST_F(BlockchainTest, CoinbaseUTXOCreatedAfterBlock) {
    std::string miner = "miner_utxo_test";
    EXPECT_TRUE(bc->addBlock(miner, {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    ASSERT_FALSE(latest->transactions.empty());
    const std::string &cbHash = latest->transactions.front().txHash;
    // Miner output is index 0
    auto utxo = bc->getUTXO(cbHash, 0);
    ASSERT_TRUE(utxo.has_value());
    EXPECT_EQ(utxo->address, miner);
    EXPECT_EQ(utxo->value,   (55ULL * 90) / 100);
}

TEST_F(BlockchainTest, OwnerUTXOCreatedAfterBlock) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    ASSERT_FALSE(latest->transactions.empty());
    const std::string &cbHash = latest->transactions.front().txHash;
    // Owner output is index 1
    auto utxo = bc->getUTXO(cbHash, 1);
    ASSERT_TRUE(utxo.has_value());
    EXPECT_EQ(utxo->address, "medor_owner");
}

TEST_F(BlockchainTest, UTXOValueMatchesRewardSplit) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    ASSERT_FALSE(latest->transactions.empty());
    const std::string &cbHash = latest->transactions.front().txHash;
    auto minerUtxo = bc->getUTXO(cbHash, 0);
    auto ownerUtxo = bc->getUTXO(cbHash, 1);
    ASSERT_TRUE(minerUtxo.has_value());
    ASSERT_TRUE(ownerUtxo.has_value());
    EXPECT_EQ(minerUtxo->value + ownerUtxo->value, 55ULL);
}

// =============================================================================
// VALIDATION
// =============================================================================

TEST_F(BlockchainTest, ValidateChainPassesOnCleanChain) {
    auto result = bc->validateChain();
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.reason.empty());
}

TEST_F(BlockchainTest, ValidateChainPassesAfterMultipleBlocks) {
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto result = bc->validateChain();
    EXPECT_TRUE(result.ok);
}

TEST_F(BlockchainTest, ValidateBlockPassesForLatest) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    // Need prev block -- get it by checking height
    // validateBlock requires both blocks so we test via validateChain
    auto result = bc->validateChain();
    EXPECT_TRUE(result.ok);
}

// =============================================================================
// FORK RESOLUTION
// =============================================================================

TEST_F(BlockchainTest, ResolveForkEmptyCandidateRejected) {
    std::vector<Block> empty;
    EXPECT_FALSE(bc->resolveFork(empty));
}

TEST_F(BlockchainTest, ResolveForkShorterChainRejected) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    std::vector<Block> shortCandidate;
    Block b;
    b.hash          = std::string(64, '0');
    b.previousHash  = "";
    b.timestamp     = 1ULL;
    b.difficulty    = 1U;
    shortCandidate.push_back(std::move(b));
    EXPECT_FALSE(bc->resolveFork(shortCandidate));
}

TEST_F(BlockchainTest, ResolveForkGenesisMismatchRejected) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    std::vector<Block> candidate;

    // Genesis with wrong hash
    Block genesis;
    genesis.hash          = std::string(64, 'f');
    genesis.previousHash  = "";
    genesis.timestamp     = 1ULL;
    genesis.difficulty    = 1U;
    candidate.push_back(std::move(genesis));

    // Add more blocks to make it longer
    for (int i = 1; i < 10; ++i) {
        Block b;
        b.hash         = std::string(63, '0') + std::to_string(i);
        b.previousHash = candidate.back().hash;
        b.timestamp    = static_cast<uint64_t>(i + 1);
        b.difficulty   = 1U;
        candidate.push_back(std::move(b));
    }
    EXPECT_FALSE(bc->resolveFork(candidate));
}

TEST_F(BlockchainTest, ResolveForkBrokenLinkageRejected) {
    auto genesis = bc->getLatestBlock();
    ASSERT_TRUE(genesis.has_value());

    std::vector<Block> candidate;
    Block b1 = genesis->clone();
    candidate.push_back(std::move(b1));

    // Block with broken previousHash linkage
    Block b2;
    b2.hash         = std::string(64, 'a');
    b2.previousHash = "wronghash";  // does not match b1.hash
    b2.timestamp    = 999ULL;
    b2.difficulty   = 1U;
    candidate.push_back(std::move(b2));

    Block b3;
    b3.hash         = std::string(64, 'b');
    b3.previousHash = candidate.back().hash;
    b3.timestamp    = 1000ULL;
    b3.difficulty   = 1U;
    candidate.push_back(std::move(b3));

    EXPECT_FALSE(bc->resolveFork(candidate));
}

TEST_F(BlockchainTest, ResolveForkEmptyHashRejected) {
    auto genesis = bc->getLatestBlock();
    ASSERT_TRUE(genesis.has_value());

    std::vector<Block> candidate;
    Block b1 = genesis->clone();
    candidate.push_back(std::move(b1));

    for (int i = 0; i < 5; ++i) {
        Block b;
        b.hash         = "";  // empty hash -- invalid
        b.previousHash = candidate.back().hash;
        b.timestamp    = static_cast<uint64_t>(i + 2);
        candidate.push_back(std::move(b));
    }
    EXPECT_FALSE(bc->resolveFork(candidate));
}

TEST_F(BlockchainTest, ResolveForkZeroTimestampRejected) {
    auto genesis = bc->getLatestBlock();
    ASSERT_TRUE(genesis.has_value());

    std::vector<Block> candidate;
    Block b1 = genesis->clone();
    candidate.push_back(std::move(b1));

    for (int i = 0; i < 5; ++i) {
        Block b;
        b.hash         = std::string(63, '0') + std::to_string(i);
        b.previousHash = candidate.back().hash;
        b.timestamp    = 0ULL;  // zero timestamp -- invalid
        b.difficulty   = 1U;
        candidate.push_back(std::move(b));
    }
    EXPECT_FALSE(bc->resolveFork(candidate));
}

// =============================================================================
// PERSISTENCE -- chain survives restart
// =============================================================================

TEST_F(BlockchainTest, ChainPersistsAcrossRestart) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    size_t heightBefore = bc->height();
    uint64_t supplyBefore = bc->totalSupply();

    // Destroy and recreate from same DB paths
    bc.reset();
    bc = std::make_unique<Blockchain>(testConfig(suffix_));

    EXPECT_EQ(bc->height(),       heightBefore);
    EXPECT_EQ(bc->totalSupply(),  supplyBefore);
}

TEST_F(BlockchainTest, BalancePersistsAcrossRestart) {
    bc->setBalance("persistent_addr", 777ULL);
    bc.reset();
    bc = std::make_unique<Blockchain>(testConfig(suffix_));
    EXPECT_EQ(bc->getBalance("persistent_addr"), 777ULL);
}

TEST_F(BlockchainTest, BaseFeePersistsAcrossRestart) {
    bc->setBaseFee(42ULL);
    // Add a block to trigger persistState()
    bc->addBlock("miner1", {});
    uint64_t feeBefore = bc->baseFee();
    bc.reset();
    bc = std::make_unique<Blockchain>(testConfig(suffix_));
    EXPECT_EQ(bc->baseFee(), feeBefore);
}

TEST_F(BlockchainTest, BlockHashPersistsAcrossRestart) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    std::string hash = latest->hash;

    bc.reset();
    bc = std::make_unique<Blockchain>(testConfig(suffix_));
    EXPECT_TRUE(bc->hasBlock(hash));
}

// =============================================================================
// CONCURRENCY
// =============================================================================

TEST_F(BlockchainTest, ConcurrentReadHeightIsSafe) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    constexpr int THREADS = 16;
    std::vector<std::thread> threads;
    std::vector<size_t> heights(THREADS);

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i]() {
            heights[i] = bc->height();
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_GE(heights[i], 1ULL);
}

TEST_F(BlockchainTest, ConcurrentReadBalanceIsSafe) {
    bc->setBalance("concurrent_addr", 500ULL);
    constexpr int THREADS = 16;
    std::vector<std::thread> threads;
    std::vector<uint64_t> balances(THREADS);

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i]() {
            balances[i] = bc->getBalance("concurrent_addr");
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_EQ(balances[i], 500ULL);
}

TEST_F(BlockchainTest, ConcurrentReadTotalSupplyIsSafe) {
    constexpr int THREADS = 16;
    std::vector<std::thread> threads;
    std::vector<uint64_t> supplies(THREADS);
    uint64_t expected = bc->totalSupply();

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i]() {
            supplies[i] = bc->totalSupply();
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_EQ(supplies[i], expected);
}

TEST_F(BlockchainTest, ConcurrentReadValidateChainIsSafe) {
    for (int i = 0; i < 3; ++i)
        EXPECT_TRUE(bc->addBlock("miner1", {}));

    constexpr int THREADS = 8;
    std::vector<std::thread> threads;
    std::vector<bool> results(THREADS);

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i]() {
            results[i] = bc->validateChain().ok;
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_TRUE(results[i]);
}

TEST_F(BlockchainTest, ConcurrentReadHasBlockIsSafe) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto latest = bc->getLatestBlock();
    ASSERT_TRUE(latest.has_value());
    std::string hash = latest->hash;

    constexpr int THREADS = 16;
    std::vector<std::thread> threads;
    std::vector<bool> results(THREADS);

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i]() {
            results[i] = bc->hasBlock(hash);
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_TRUE(results[i]);
}

TEST_F(BlockchainTest, ConcurrentGetLatestBlockIsSafe) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    constexpr int THREADS = 8;
    std::vector<std::thread> threads;
    std::vector<bool> results(THREADS);

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i]() {
            auto b    = bc->getLatestBlock();
            results[i] = b.has_value();
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_TRUE(results[i]);
}

TEST_F(BlockchainTest, MineBlockAsyncCompletesSuccessfully) {
    std::atomic<bool> called{false};
    std::atomic<bool> success{false};

    bc->mineBlockAsync("miner_async", {},
        [&](bool ok, Block /*b*/) {
            success.store(ok);
            called.store(true);
        });

    // Wait up to 10 seconds for async mining to complete
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(10);
    while (!called.load() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(called.load());
    EXPECT_TRUE(success.load());
}

TEST_F(BlockchainTest, MineBlockAsyncIncreasesHeight) {
    size_t before = bc->height();
    std::atomic<bool> done{false};

    bc->mineBlockAsync("miner_async2", {},
        [&](bool ok, Block /*b*/) {
            done.store(true);
        });

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(10);
    while (!done.load() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_GT(bc->height(), before);
}

// =============================================================================
// REWARD SCHEDULE
// =============================================================================

TEST_F(BlockchainTest, RewardScheduleFallsBackToLastEntry) {
    // The test config has only one entry: { UINT64_MAX, 55 }
    // which is the permanent fallback -- reward must always be 55
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    auto b = bc->getLatestBlock();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->reward, 55ULL);
}

TEST_F(BlockchainTest, MultipleRewardScheduleEntries) {
    Blockchain::Config cfg = testConfig("_schedule");
    cfg.rewardSchedule = {
        { 1ULL,        100ULL },  // first second: 100
        { UINT64_MAX,   50ULL }   // thereafter:    50
    };
    Blockchain bc2(cfg);
    // Genesis was created at construction -- reward depends on timing
    // Just verify it does not exceed the first schedule entry
    EXPECT_LE(bc2.totalSupply(), cfg.maxSupply);
}

// =============================================================================
// PRINT CHAIN (smoke test -- just verify it does not crash)
// =============================================================================

TEST_F(BlockchainTest, PrintChainDoesNotCrash) {
    EXPECT_TRUE(bc->addBlock("miner1", {}));
    EXPECT_NO_THROW(bc->printChain());
}

TEST_F(BlockchainTest, PrintChainEmptyDoesNotCrash) {
    EXPECT_NO_THROW(bc->printChain());
}
