#pragma once

#include "accountdb.h"
#include "db/blockdb.h"
#include "utxo.h"
#include "block.h"
#include "transaction.h"
#include "crypto/keccak256.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

class Blockchain {
public:

    struct Config {
        std::string ownerAddress;
        std::string treasuryAddress   = "medor_treasury";
        std::string blockDBPath       = "data/medorcoin_blocks";
        std::string accountDBPath     = "data/medorcoin_accounts";
        std::string validatorKeyDir   = "data/validator_keys";
        uint64_t    maxSupply         = 50000000ULL;
        uint32_t    initialMedor      = 4;
        uint64_t    initialBaseFee    = 1ULL;
        uint64_t    targetGasPerBlock = 1000000ULL;
        std::vector<std::pair<uint64_t, uint64_t>> rewardSchedule = {
            { 60ULL * 24ULL * 60ULL * 60ULL, 55ULL },
            { UINT64_MAX,                    30ULL }
        };
    };

    struct ValidationResult {
        bool        ok          = false;
        size_t      failedIndex = 0;
        std::string reason;
    };

    using MinedCallback = std::function<void(bool success, Block minedBlock)>;

    explicit Blockchain(Config cfg);
    ~Blockchain();

    Blockchain(const Blockchain &)            = delete;
    Blockchain &operator=(const Blockchain &) = delete;

    size_t   height()      const noexcept;
    uint64_t totalSupply() const noexcept;
    uint64_t baseFee()     const noexcept;
    bool     isOpen()      const noexcept;

    bool                 hasBlock(const std::string &hash) const noexcept;
    std::optional<Block> getLatestBlock()                  const noexcept;

    uint64_t getBalance   (const std::string &addr)                  const noexcept;
    bool     setBalance   (const std::string &addr, uint64_t amount)       noexcept;
    bool     addBalance   (const std::string &addr, uint64_t amount)       noexcept;
    bool     deductBalance(const std::string &addr, uint64_t amount)       noexcept;

    void setBaseFee   (uint64_t fee)                        noexcept;
    void burnBaseFees (uint64_t amount)                     noexcept;
    void adjustBaseFee(uint64_t gasUsed, uint64_t gasLimit) noexcept;

    bool addBlock(const std::string       &minerAddr,
                  std::vector<Transaction> transactions) noexcept;

    void mineBlockAsync(const std::string       &minerAddr,
                        std::vector<Transaction> transactions,
                        MinedCallback            callback);

    ValidationResult validateBlock(const Block &block,
                                   const Block &prevBlock) const noexcept;
    ValidationResult validateChain()                        const noexcept;

    bool resolveFork(const std::vector<Block> &candidateChain) noexcept;

    std::optional<UTXO> getUTXO(const std::string &txHash,
                                 int                outputIndex) const noexcept;

    void printChain() const noexcept;

private:
    Config    cfg_;
    BlockDB   blockDB_;
    AccountDB accountDB_;
    UTXOSet   utxoSet_;

    std::vector<Block>        chain_;
    mutable std::shared_mutex rwMutex_;

    std::atomic<uint64_t> totalSupply_{ 0 };
    std::atomic<uint64_t> baseFeePerGas_{ 1 };
    bool                  initialised_ = false;

    static constexpr const char *KEY_TOTAL_SUPPLY = "__chain_total_supply__";
    static constexpr const char *KEY_BASE_FEE     = "__chain_base_fee__";

    bool     loadChainFromDB()                                      noexcept;
    bool     loadPersistedState()                                   noexcept;
    bool     persistState()                                         noexcept;
    uint64_t calculateReward()                                const noexcept;
    bool     validateTransactions(
                 const std::vector<Transaction> &txs,
                 const std::string              &minerAddr,
                 uint64_t                        blockHeight) const noexcept;
    bool     hasDuplicateTxInBlock(
                 const std::vector<Transaction> &txs)         const noexcept;
    bool     applyBlockToState(const Block &block,
                                uint64_t    blockHeight)            noexcept;
    void     rollbackBlockFromState(const Block &block)             noexcept;
};
