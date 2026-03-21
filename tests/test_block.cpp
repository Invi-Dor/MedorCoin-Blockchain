#include <gtest/gtest.h>
#include "block.h"
#include "transaction.h"

TEST(BlockTest, DefaultConstructor) {
    Block b;
    EXPECT_TRUE(b.hash.empty());
    EXPECT_TRUE(b.previousHash.empty());
    EXPECT_EQ(b.nonce, 0ULL);
    EXPECT_EQ(b.timestamp, 0ULL);
    EXPECT_EQ(b.difficulty, 0U);
    EXPECT_EQ(b.reward, 0ULL);
    EXPECT_EQ(b.baseFee, 0ULL);
    EXPECT_EQ(b.gasUsed, 0ULL);
    EXPECT_EQ(b.gasLimit, 0ULL);
    EXPECT_TRUE(b.transactions.empty());
}

TEST(BlockTest, ParameterizedConstructor) {
    Block b("prevhash", "test data", 4, "miner123");
    EXPECT_EQ(b.previousHash, "prevhash");
    EXPECT_EQ(b.data, "test data");
    EXPECT_EQ(b.difficulty, 4U);
    EXPECT_EQ(b.minerAddress, "miner123");
}

TEST(BlockTest, CloneIsIndependent) {
    Block b("prev", "data", 2, "miner");
    b.hash      = "abc123";
    b.timestamp = 999ULL;
    b.reward    = 55ULL;

    Block c = b.clone();
    EXPECT_EQ(c.hash,         b.hash);
    EXPECT_EQ(c.previousHash, b.previousHash);
    EXPECT_EQ(c.timestamp,    b.timestamp);
    EXPECT_EQ(c.reward,       b.reward);
    EXPECT_EQ(c.minerAddress, b.minerAddress);

    // Mutating clone does not affect original
    c.hash = "different";
    EXPECT_EQ(b.hash, "abc123");
}

TEST(BlockTest, ClearHash) {
    Block b("prev", "data", 2, "miner");
    b.hash      = "somehash";
    b.signature = "sig";
    b.clearHash();
    EXPECT_TRUE(b.hash.empty());
    EXPECT_TRUE(b.signature.empty());
}

TEST(BlockTest, HasHash) {
    Block b;
    EXPECT_FALSE(b.hasHash());
    b.hash = "abc";
    EXPECT_TRUE(b.hasHash());
}

TEST(BlockTest, MoveConstructor) {
    Block b("prev", "data", 3, "miner");
    b.hash      = "hash1";
    b.timestamp = 42ULL;

    Block moved = std::move(b);
    EXPECT_EQ(moved.hash,      "hash1");
    EXPECT_EQ(moved.timestamp, 42ULL);
}

TEST(BlockTest, SerializeDeserializeRoundTrip) {
    Block b("prevhash", "block data", 2, "minerAddr");
    b.timestamp = 1000ULL;
    b.reward    = 55ULL;
    b.baseFee   = 1ULL;
    b.nonce     = 12345ULL;
    b.hash      = "deadbeef";

    std::string serialized = b.serialize();
    EXPECT_FALSE(serialized.empty());

    Block b2;
    EXPECT_TRUE(b2.deserialize(serialized));
    EXPECT_EQ(b2.previousHash, b.previousHash);
    EXPECT_EQ(b2.data,         b.data);
    EXPECT_EQ(b2.difficulty,   b.difficulty);
    EXPECT_EQ(b2.minerAddress, b.minerAddress);
    EXPECT_EQ(b2.timestamp,    b.timestamp);
    EXPECT_EQ(b2.reward,       b.reward);
    EXPECT_EQ(b2.nonce,        b.nonce);
    EXPECT_EQ(b2.hash,         b.hash);
}

TEST(BlockTest, DeserializeEmptyFails) {
    Block b;
    EXPECT_FALSE(b.deserialize(""));
}

TEST(BlockTest, MaxTransactionsLimit) {
    EXPECT_GT(Block::MAX_TRANSACTIONS, 0U);
    EXPECT_LE(Block::MAX_TRANSACTIONS, 100000U);
}

TEST(BlockTest, CopyConstructorDeleted) {
    EXPECT_FALSE(std::is_copy_constructible<Block>::value);
}
