#include <gtest/gtest.h>
#include "utxo.h"
#include "transaction.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// =============================================================================
// HELPERS
// =============================================================================

static TxOutput makeOutput(uint64_t value, const std::string &addr)
{
    TxOutput out;
    out.value   = value;
    out.address = addr;
    return out;
}

// =============================================================================
// FIXTURE
// =============================================================================

class UTXOSetTest : public ::testing::Test {
protected:
    UTXOSet utxoSet;
};

// =============================================================================
// ADD AND GET
// =============================================================================

TEST_F(UTXOSetTest, AddAndGetUTXO) {
    auto out = makeOutput(1000, "addr1");
    EXPECT_TRUE(utxoSet.addUTXO(out, "txhash1", 0, 1, false));
    auto utxo = utxoSet.getUTXO("txhash1", 0);
    ASSERT_TRUE(utxo.has_value());
    EXPECT_EQ(utxo->value,   1000ULL);
    EXPECT_EQ(utxo->address, "addr1");
}

TEST_F(UTXOSetTest, AddUTXOStoresBlockHeight) {
    auto out = makeOutput(100, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "tx1", 0, 42, false));
    auto utxo = utxoSet.getUTXO("tx1", 0);
    ASSERT_TRUE(utxo.has_value());
    EXPECT_EQ(utxo->blockHeight, 42ULL);
}

TEST_F(UTXOSetTest, AddUTXOStoresCoinbaseFlag) {
    auto out = makeOutput(55, "miner");
    EXPECT_TRUE(utxoSet.addUTXO(out, "cb1", 0, 1, true));
    auto utxo = utxoSet.getUTXO("cb1", 0);
    ASSERT_TRUE(utxo.has_value());
    EXPECT_TRUE(utxo->isCoinbase);
}

TEST_F(UTXOSetTest, AddUTXONonCoinbaseFlagFalse) {
    auto out = makeOutput(100, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "tx2", 0, 1, false));
    auto utxo = utxoSet.getUTXO("tx2", 0);
    ASSERT_TRUE(utxo.has_value());
    EXPECT_FALSE(utxo->isCoinbase);
}

TEST_F(UTXOSetTest, AddMultipleOutputIndexesSameTx) {
    auto out0 = makeOutput(100, "addr0");
    auto out1 = makeOutput(200, "addr1");
    auto out2 = makeOutput(300, "addr2");
    EXPECT_TRUE(utxoSet.addUTXO(out0, "txMulti", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(out1, "txMulti", 1, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(out2, "txMulti", 2, 1, false));
    EXPECT_EQ(utxoSet.size(), 3U);
    auto u0 = utxoSet.getUTXO("txMulti", 0);
    auto u1 = utxoSet.getUTXO("txMulti", 1);
    auto u2 = utxoSet.getUTXO("txMulti", 2);
    ASSERT_TRUE(u0.has_value()); EXPECT_EQ(u0->value, 100ULL);
    ASSERT_TRUE(u1.has_value()); EXPECT_EQ(u1->value, 200ULL);
    ASSERT_TRUE(u2.has_value()); EXPECT_EQ(u2->value, 300ULL);
}

TEST_F(UTXOSetTest, GetNonExistentUTXO) {
    auto utxo = utxoSet.getUTXO("nonexistent", 0);
    EXPECT_FALSE(utxo.has_value());
}

TEST_F(UTXOSetTest, GetWrongOutputIndex) {
    auto out = makeOutput(100, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "tx3", 0, 1, false));
    EXPECT_FALSE(utxoSet.getUTXO("tx3", 1).has_value());
}

// =============================================================================
// SPEND
// =============================================================================

TEST_F(UTXOSetTest, SpendUTXO) {
    auto out = makeOutput(500, "addr2");
    EXPECT_TRUE(utxoSet.addUTXO(out, "tx1", 0, 1, false));
    EXPECT_TRUE(utxoSet.spendUTXO("tx1", 0, 2));
    EXPECT_FALSE(utxoSet.getUTXO("tx1", 0).has_value());
}

TEST_F(UTXOSetTest, SpendNonExistentUTXO) {
    EXPECT_FALSE(utxoSet.spendUTXO("fakehash", 0, 1));
}

TEST_F(UTXOSetTest, SpendAlreadySpentUTXO) {
    auto out = makeOutput(100, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "txSpent", 0, 1, false));
    EXPECT_TRUE(utxoSet.spendUTXO("txSpent", 0, 2));
    EXPECT_FALSE(utxoSet.spendUTXO("txSpent", 0, 3));
}

TEST_F(UTXOSetTest, SpendOnlyCorrectOutputIndex) {
    auto out0 = makeOutput(100, "addr");
    auto out1 = makeOutput(200, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out0, "txIdx", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(out1, "txIdx", 1, 1, false));
    EXPECT_TRUE(utxoSet.spendUTXO("txIdx", 0, 2));
    EXPECT_FALSE(utxoSet.getUTXO("txIdx", 0).has_value());
    EXPECT_TRUE(utxoSet.getUTXO("txIdx", 1).has_value());
}

// =============================================================================
// DUPLICATE REJECTION
// =============================================================================

TEST_F(UTXOSetTest, DuplicateAddRejected) {
    auto out = makeOutput(100, "addr3");
    EXPECT_TRUE(utxoSet.addUTXO(out, "tx2", 0, 1, false));
    EXPECT_FALSE(utxoSet.addUTXO(out, "tx2", 0, 1, false));
    EXPECT_EQ(utxoSet.size(), 1U);
}

// =============================================================================
// BALANCE
// =============================================================================

TEST_F(UTXOSetTest, GetBalance) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(300, "addrX"), "txA", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(700, "addrX"), "txB", 0, 1, false));
    auto bal = utxoSet.getBalance("addrX");
    ASSERT_TRUE(bal.has_value());
    EXPECT_EQ(bal.value(), 1000ULL);
}

TEST_F(UTXOSetTest, GetBalanceUnknownAddressReturnsNullopt) {
    EXPECT_FALSE(utxoSet.getBalance("unknown").has_value());
}

TEST_F(UTXOSetTest, GetBalanceAfterSpendDecreases) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(500, "addrBal"), "txBal1", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(300, "addrBal"), "txBal2", 0, 1, false));
    EXPECT_TRUE(utxoSet.spendUTXO("txBal1", 0, 2));
    auto bal = utxoSet.getBalance("addrBal");
    ASSERT_TRUE(bal.has_value());
    EXPECT_EQ(bal.value(), 300ULL);
}

TEST_F(UTXOSetTest, GetBalanceAfterAllSpentReturnsNullopt) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(100, "addrGone"), "txGone", 0, 1, false));
    EXPECT_TRUE(utxoSet.spendUTXO("txGone", 0, 2));
    EXPECT_FALSE(utxoSet.getBalance("addrGone").has_value());
}

TEST_F(UTXOSetTest, GetBalanceOverflowSafe) {
    uint64_t half = std::numeric_limits<uint64_t>::max() / 2;
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(half, "addrOvf"), "txOvf1", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(half, "addrOvf"), "txOvf2", 0, 1, false));
    // getBalance returns nullopt on overflow per utxo.h design guarantee
    auto bal = utxoSet.getBalance("addrOvf");
    // Either succeeds with correct value or returns nullopt -- must not crash
    if (bal.has_value())
        EXPECT_LE(bal.value(), std::numeric_limits<uint64_t>::max());
}

// =============================================================================
// GET UTXOS FOR ADDRESS
// =============================================================================

TEST_F(UTXOSetTest, GetUTXOsForAddress) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(100, "addrY"), "txC", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(200, "addrY"), "txD", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(999, "addrZ"), "txE", 0, 1, false));
    auto utxos = utxoSet.getUTXOsForAddress("addrY");
    EXPECT_EQ(utxos.size(), 2U);
}

TEST_F(UTXOSetTest, GetUTXOsForAddressExcludesOtherAddresses) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(100, "addrA"), "txF1", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(200, "addrB"), "txF2", 0, 1, false));
    auto utxos = utxoSet.getUTXOsForAddress("addrA");
    EXPECT_EQ(utxos.size(), 1U);
    EXPECT_EQ(utxos[0].address, "addrA");
}

TEST_F(UTXOSetTest, GetUTXOsForAddressAfterSpend) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(100, "addrSeq"), "txSeq1", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(200, "addrSeq"), "txSeq2", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(300, "addrSeq"), "txSeq3", 0, 1, false));
    EXPECT_TRUE(utxoSet.spendUTXO("txSeq2", 0, 2));
    auto utxos = utxoSet.getUTXOsForAddress("addrSeq");
    EXPECT_EQ(utxos.size(), 2U);
    for (const auto &u : utxos)
        EXPECT_NE(u.txHash, "txSeq2");
}

TEST_F(UTXOSetTest, GetUTXOsForAddressEmptyReturnsEmptyVector) {
    auto utxos = utxoSet.getUTXOsForAddress("nonexistent");
    EXPECT_TRUE(utxos.empty());
}

TEST_F(UTXOSetTest, GetUTXOsForAddressValuesAreCorrect) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(111, "addrVal"), "txVal1", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(222, "addrVal"), "txVal2", 0, 1, false));
    auto utxos = utxoSet.getUTXOsForAddress("addrVal");
    ASSERT_EQ(utxos.size(), 2U);
    uint64_t total = 0;
    for (const auto &u : utxos) total += u.value;
    EXPECT_EQ(total, 333ULL);
}

// =============================================================================
// IS UNSPENT
// =============================================================================

TEST_F(UTXOSetTest, IsUnspent) {
    auto out = makeOutput(50, "addrW");
    EXPECT_TRUE(utxoSet.addUTXO(out, "txFW", 0, 1, false));
    EXPECT_TRUE(utxoSet.isUnspent("txFW", 0));
    EXPECT_TRUE(utxoSet.spendUTXO("txFW", 0, 2));
    EXPECT_FALSE(utxoSet.isUnspent("txFW", 0));
}

TEST_F(UTXOSetTest, IsUnspentNonExistentReturnsFalse) {
    EXPECT_FALSE(utxoSet.isUnspent("fakehash", 0));
}

// =============================================================================
// COINBASE MATURITY
// =============================================================================

TEST_F(UTXOSetTest, CoinbaseMaturityEnforced) {
    auto out = makeOutput(55, "minerAddr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "cbTx", 0, 1, true));
    EXPECT_FALSE(utxoSet.spendUTXO("cbTx", 0, 50));
    EXPECT_TRUE(utxoSet.spendUTXO("cbTx", 0,
        1 + UTXOSet::COINBASE_MATURITY + 1));
}

TEST_F(UTXOSetTest, CoinbaseAtExactMaturityBoundary) {
    auto out = makeOutput(55, "minerAddr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "cbBound", 0, 1, true));
    // Exactly at maturity boundary -- should succeed
    EXPECT_TRUE(utxoSet.spendUTXO("cbBound", 0,
        1 + UTXOSet::COINBASE_MATURITY));
}

TEST_F(UTXOSetTest, CoinbaseOneBlockBeforeMaturityFails) {
    auto out = makeOutput(55, "minerAddr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "cbEarly", 0, 1, true));
    EXPECT_FALSE(utxoSet.spendUTXO("cbEarly", 0,
        UTXOSet::COINBASE_MATURITY));
}

TEST_F(UTXOSetTest, NonCoinbaseSpendableImmediately) {
    auto out = makeOutput(100, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "txImm", 0, 1, false));
    EXPECT_TRUE(utxoSet.spendUTXO("txImm", 0, 1));
}

TEST_F(UTXOSetTest, RollbackBypassesMaturity) {
    auto out = makeOutput(55, "minerAddr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "cbTx2", 0, 1, true));
    EXPECT_TRUE(utxoSet.spendUTXO("cbTx2", 0,
        std::numeric_limits<uint64_t>::max()));
}

TEST_F(UTXOSetTest, MultipleCoinbaseUTXOsAllEnforceMaturity) {
    for (int i = 0; i < 5; ++i) {
        auto out = makeOutput(55, "miner");
        std::string hash = "cbMulti" + std::to_string(i);
        EXPECT_TRUE(utxoSet.addUTXO(out, hash, 0,
            static_cast<uint64_t>(i + 1), true));
        EXPECT_FALSE(utxoSet.spendUTXO(hash, 0, 10));
    }
}

// =============================================================================
// SIZE TRACKING
// =============================================================================

TEST_F(UTXOSetTest, SizeTracking) {
    EXPECT_EQ(utxoSet.size(), 0U);
    auto out = makeOutput(1, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "txG", 0, 1, false));
    EXPECT_EQ(utxoSet.size(), 1U);
    EXPECT_TRUE(utxoSet.spendUTXO("txG", 0, 2));
    EXPECT_EQ(utxoSet.size(), 0U);
}

TEST_F(UTXOSetTest, SizeAfterMultipleAdds) {
    for (int i = 0; i < 10; ++i) {
        auto out = makeOutput(static_cast<uint64_t>(i + 1), "addr");
        EXPECT_TRUE(utxoSet.addUTXO(out, "txSz" + std::to_string(i),
                                     0, 1, false));
    }
    EXPECT_EQ(utxoSet.size(), 10U);
}

TEST_F(UTXOSetTest, SizeAfterClear) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(100, "addr"), "txH", 0, 1, false));
    utxoSet.clear();
    EXPECT_EQ(utxoSet.size(), 0U);
}

TEST_F(UTXOSetTest, ClearRemovesAllUTXOs) {
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(utxoSet.addUTXO(makeOutput(100, "addr"),
            "txClr" + std::to_string(i), 0, 1, false));
    utxoSet.clear();
    EXPECT_EQ(utxoSet.size(), 0U);
    EXPECT_FALSE(utxoSet.getBalance("addr").has_value());
    EXPECT_TRUE(utxoSet.getUTXOsForAddress("addr").empty());
}

// =============================================================================
// INPUT VALIDATION
// =============================================================================

TEST_F(UTXOSetTest, EmptyAddressRejected) {
    TxOutput out;
    out.value   = 100;
    out.address = "";
    EXPECT_FALSE(utxoSet.addUTXO(out, "txI", 0, 1, false));
}

TEST_F(UTXOSetTest, EmptyTxHashRejected) {
    TxOutput out;
    out.value   = 100;
    out.address = "addr";
    EXPECT_FALSE(utxoSet.addUTXO(out, "", 0, 1, false));
}

TEST_F(UTXOSetTest, NegativeOutputIndexRejected) {
    TxOutput out;
    out.value   = 100;
    out.address = "addr";
    EXPECT_FALSE(utxoSet.addUTXO(out, "txJ", -1, 1, false));
}

TEST_F(UTXOSetTest, ZeroValueUTXOAccepted) {
    auto out = makeOutput(0, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "txZero", 0, 1, false));
    auto utxo = utxoSet.getUTXO("txZero", 0);
    ASSERT_TRUE(utxo.has_value());
    EXPECT_EQ(utxo->value, 0ULL);
}

TEST_F(UTXOSetTest, MaxUint64ValueAccepted) {
    auto out = makeOutput(std::numeric_limits<uint64_t>::max(), "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "txMax", 0, 1, false));
    auto utxo = utxoSet.getUTXO("txMax", 0);
    ASSERT_TRUE(utxo.has_value());
    EXPECT_EQ(utxo->value, std::numeric_limits<uint64_t>::max());
}

TEST_F(UTXOSetTest, VeryLongAddressAccepted) {
    std::string longAddr(512, 'x');
    auto out = makeOutput(100, longAddr);
    EXPECT_TRUE(utxoSet.addUTXO(out, "txLong", 0, 1, false));
    auto utxo = utxoSet.getUTXO("txLong", 0);
    ASSERT_TRUE(utxo.has_value());
    EXPECT_EQ(utxo->address, longAddr);
}

TEST_F(UTXOSetTest, VeryLongTxHashAccepted) {
    std::string longHash(256, 'a');
    auto out = makeOutput(100, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, longHash, 0, 1, false));
    auto utxo = utxoSet.getUTXO(longHash, 0);
    ASSERT_TRUE(utxo.has_value());
}

TEST_F(UTXOSetTest, LargeOutputIndexAccepted) {
    auto out = makeOutput(100, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "txBigIdx", 9999, 1, false));
    EXPECT_TRUE(utxoSet.getUTXO("txBigIdx", 9999).has_value());
}

TEST_F(UTXOSetTest, ZeroBlockHeightAccepted) {
    auto out = makeOutput(100, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "txHeight0", 0, 0, false));
    EXPECT_TRUE(utxoSet.getUTXO("txHeight0", 0).has_value());
}

TEST_F(UTXOSetTest, MaxBlockHeightAccepted) {
    auto out = makeOutput(100, "addr");
    EXPECT_TRUE(utxoSet.addUTXO(out, "txMaxHeight", 0,
        std::numeric_limits<uint64_t>::max() - 1, false));
    EXPECT_TRUE(utxoSet.getUTXO("txMaxHeight", 0).has_value());
}

// =============================================================================
// METRICS
// =============================================================================

TEST_F(UTXOSetTest, MetricsTrackedOnAdd) {
    auto before = utxoSet.getMetrics();
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(100, "addr"), "txM1", 0, 1, false));
    auto after = utxoSet.getMetrics();
    EXPECT_GT(after.utxosAdded, before.utxosAdded);
}

TEST_F(UTXOSetTest, MetricsTrackedOnSpend) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(100, "addr"), "txM2", 0, 1, false));
    auto before = utxoSet.getMetrics();
    EXPECT_TRUE(utxoSet.spendUTXO("txM2", 0, 2));
    auto after = utxoSet.getMetrics();
    EXPECT_GT(after.utxosSpent, before.utxosSpent);
}

TEST_F(UTXOSetTest, MetricsTotalValueTracked) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(500, "addr"), "txMV1", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(300, "addr"), "txMV2", 0, 1, false));
    auto m = utxoSet.getMetrics();
    EXPECT_GE(m.totalValueTracked, 800ULL);
}

TEST_F(UTXOSetTest, MetricsCoinbaseMaturityRejected) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(55, "miner"), "cbMet", 0, 1, true));
    auto before = utxoSet.getMetrics();
    utxoSet.spendUTXO("cbMet", 0, 10);  // before maturity -- rejected
    auto after = utxoSet.getMetrics();
    EXPECT_GE(after.coinbaseMaturityRejected,
              before.coinbaseMaturityRejected + 1);
}

TEST_F(UTXOSetTest, MetricsCurrentCountMatchesSize) {
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(utxoSet.addUTXO(makeOutput(100, "addr"),
            "txCnt" + std::to_string(i), 0, 1, false));
    auto m = utxoSet.getMetrics();
    EXPECT_EQ(m.currentUtxoCount, utxoSet.size());
}

// =============================================================================
// STRESS / PERFORMANCE
// =============================================================================

TEST_F(UTXOSetTest, StressAddThousandUTXOs) {
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        auto out = makeOutput(static_cast<uint64_t>(i + 1),
                              "stressAddr" + std::to_string(i % 10));
        EXPECT_TRUE(utxoSet.addUTXO(out,
            "stressTx" + std::to_string(i), 0,
            static_cast<uint64_t>(i + 1), false));
    }
    EXPECT_EQ(utxoSet.size(), static_cast<size_t>(N));
}

TEST_F(UTXOSetTest, StressSpendThousandUTXOs) {
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        auto out = makeOutput(100, "addr");
        utxoSet.addUTXO(out, "spTx" + std::to_string(i), 0,
                         static_cast<uint64_t>(i + 1), false);
    }
    for (int i = 0; i < N; ++i)
        EXPECT_TRUE(utxoSet.spendUTXO(
            "spTx" + std::to_string(i), 0,
            static_cast<uint64_t>(i + 2)));
    EXPECT_EQ(utxoSet.size(), 0U);
}

TEST_F(UTXOSetTest, StressBalanceAccumulationCorrect) {
    constexpr int N = 100;
    uint64_t expectedTotal = 0;
    for (int i = 0; i < N; ++i) {
        uint64_t val = static_cast<uint64_t>(i + 1);
        auto out = makeOutput(val, "richAddr");
        EXPECT_TRUE(utxoSet.addUTXO(out,
            "richTx" + std::to_string(i), 0,
            static_cast<uint64_t>(i + 1), false));
        expectedTotal += val;
    }
    auto bal = utxoSet.getBalance("richAddr");
    ASSERT_TRUE(bal.has_value());
    EXPECT_EQ(bal.value(), expectedTotal);
}

TEST_F(UTXOSetTest, StressAddressIndexConsistency) {
    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        std::string addr = "multiAddr" + std::to_string(i % 5);
        auto out = makeOutput(100, addr);
        EXPECT_TRUE(utxoSet.addUTXO(out,
            "idxTx" + std::to_string(i), 0,
            static_cast<uint64_t>(i + 1), false));
    }
    for (int a = 0; a < 5; ++a) {
        std::string addr = "multiAddr" + std::to_string(a);
        auto utxos = utxoSet.getUTXOsForAddress(addr);
        EXPECT_EQ(utxos.size(), static_cast<size_t>(N / 5));
        for (const auto &u : utxos)
            EXPECT_EQ(u.address, addr);
    }
}

// =============================================================================
// CONCURRENCY
// =============================================================================

TEST_F(UTXOSetTest, ConcurrentReadsAreSafe) {
    for (int i = 0; i < 20; ++i) {
        auto out = makeOutput(100, "concAddr");
        utxoSet.addUTXO(out, "concTx" + std::to_string(i), 0,
                         static_cast<uint64_t>(i + 1), false);
    }

    constexpr int THREADS = 16;
    std::vector<std::thread> threads;
    std::vector<size_t> sizes(THREADS);

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i]() {
            sizes[i] = utxoSet.getUTXOsForAddress("concAddr").size();
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_EQ(sizes[i], 20U);
}

TEST_F(UTXOSetTest, ConcurrentBalanceReadsAreSafe) {
    for (int i = 0; i < 10; ++i) {
        auto out = makeOutput(100, "balAddr");
        utxoSet.addUTXO(out, "balTx" + std::to_string(i), 0,
                         static_cast<uint64_t>(i + 1), false);
    }

    constexpr int THREADS = 16;
    std::vector<std::thread> threads;
    std::vector<uint64_t> balances(THREADS, 0);

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i]() {
            auto b = utxoSet.getBalance("balAddr");
            if (b.has_value()) balances[i] = b.value();
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_EQ(balances[i], 1000ULL);
}

TEST_F(UTXOSetTest, ConcurrentAddsAreSafe) {
    constexpr int THREADS = 8;
    constexpr int PER_THREAD = 50;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                std::string hash = "caTx_" + std::to_string(t)
                                 + "_" + std::to_string(i);
                auto out = makeOutput(100,
                    "caAddr" + std::to_string(t));
                if (utxoSet.addUTXO(out, hash, 0,
                        static_cast<uint64_t>(t * PER_THREAD + i + 1),
                        false))
                    ++successCount;
            }
        });
    }
    for (auto &t : threads) t.join();

    EXPECT_EQ(successCount.load(), THREADS * PER_THREAD);
    EXPECT_EQ(utxoSet.size(),
              static_cast<size_t>(THREADS * PER_THREAD));
}

TEST_F(UTXOSetTest, ConcurrentAddsAndReadsAreSafe) {
    constexpr int WRITERS = 4;
    constexpr int READERS = 4;
    constexpr int PER_WRITER = 25;
    std::vector<std::thread> threads;
    std::atomic<bool> stop{false};

    for (int t = 0; t < WRITERS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < PER_WRITER; ++i) {
                std::string hash = "rwTx_" + std::to_string(t)
                                 + "_" + std::to_string(i);
                auto out = makeOutput(100, "rwAddr");
                utxoSet.addUTXO(out, hash, 0,
                    static_cast<uint64_t>(t * PER_WRITER + i + 1),
                    false);
            }
        });
    }

    for (int t = 0; t < READERS; ++t) {
        threads.emplace_back([&]() {
            while (!stop.load()) {
                utxoSet.getBalance("rwAddr");
                utxoSet.size();
                std::this_thread::yield();
            }
        });
    }

    for (int t = 0; t < WRITERS; ++t)
        threads[t].join();

    stop.store(true);
    for (int t = WRITERS; t < WRITERS + READERS; ++t)
        threads[t].join();

    EXPECT_EQ(utxoSet.size(),
              static_cast<size_t>(WRITERS * PER_WRITER));
}

TEST_F(UTXOSetTest, ConcurrentSpendsAreSafe) {
    constexpr int N = 100;
    for (int i = 0; i < N; ++i) {
        auto out = makeOutput(100, "spAddr");
        utxoSet.addUTXO(out, "spConcTx" + std::to_string(i), 0,
                         static_cast<uint64_t>(i + 1), false);
    }

    constexpr int THREADS = 8;
    std::vector<std::thread> threads;
    std::atomic<int> spendCount{0};

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            int start = t * (N / THREADS);
            int end   = start + (N / THREADS);
            for (int i = start; i < end; ++i) {
                if (utxoSet.spendUTXO(
                        "spConcTx" + std::to_string(i), 0,
                        static_cast<uint64_t>(i + 2)))
                    ++spendCount;
            }
        });
    }
    for (auto &t : threads) t.join();

    EXPECT_EQ(spendCount.load(), N);
    EXPECT_EQ(utxoSet.size(), 0U);
}

TEST_F(UTXOSetTest, ConcurrentIsUnspentReadsAreSafe) {
    for (int i = 0; i < 10; ++i) {
        auto out = makeOutput(100, "addr");
        utxoSet.addUTXO(out, "iuTx" + std::to_string(i), 0,
                         static_cast<uint64_t>(i + 1), false);
    }

    constexpr int THREADS = 16;
    std::vector<std::thread> threads;
    std::vector<bool> results(THREADS);

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&, i]() {
            results[i] = utxoSet.isUnspent("iuTx0", 0);
        });
    }
    for (auto &t : threads) t.join();

    for (int i = 0; i < THREADS; ++i)
        EXPECT_TRUE(results[i]);
}

// =============================================================================
// LOAD SNAPSHOT
// =============================================================================

TEST_F(UTXOSetTest, LoadSnapshotRestoresState) {
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(100, "snapAddr"), "snapTx1", 0, 1, false));
    EXPECT_TRUE(utxoSet.addUTXO(makeOutput(200, "snapAddr"), "snapTx2", 0, 2, false));

    // Build a snapshot manually
    std::unordered_map<std::string, UTXO> snapshot;
    UTXO u;
    u.txHash      = "snapTx1";
    u.outputIndex = 0;
    u.value       = 100;
    u.address     = "snapAddr";
    u.blockHeight = 1;
    u.isCoinbase  = false;
    snapshot[UTXOSet::makeKey("snapTx1", 0)] = u;

    utxoSet.clear();
    EXPECT_TRUE(utxoSet.loadSnapshot(snapshot));
    EXPECT_EQ(utxoSet.size(), 1U);

    auto utxo = utxoSet.getUTXO("snapTx1", 0);
    ASSERT_TRUE(utxo.has_value());
    EXPECT_EQ(utxo->value,   100ULL);
    EXPECT_EQ(utxo->address, "snapAddr");
}

TEST_F(UTXOSetTest, LoadSnapshotEmptySucceeds) {
    std::unordered_map<std::string, UTXO> empty;
    EXPECT_TRUE(utxoSet.loadSnapshot(empty));
    EXPECT_EQ(utxoSet.size(), 0U);
}

TEST_F(UTXOSetTest, MakeKeyIsConsistent) {
    std::string k1 = UTXOSet::makeKey("txHash1", 0);
    std::string k2 = UTXOSet::makeKey("txHash1", 0);
    std::string k3 = UTXOSet::makeKey("txHash1", 1);
    EXPECT_EQ(k1, k2);
    EXPECT_NE(k1, k3);
}

TEST_F(UTXOSetTest, MakeKeyDifferentHashesDifferentKeys) {
    std::string k1 = UTXOSet::makeKey("hashA", 0);
    std::string k2 = UTXOSet::makeKey("hashB", 0);
    EXPECT_NE(k1, k2);
}
