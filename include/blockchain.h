#pragma once

// ── DO NOT include blockchain.cpp here — headers never include .cpp files.
// blockchain.cpp includes blockchain.h, not the other way around.

#include “accountdb.h”
#include “blockdb.h”
#include “utxo.h”
#include “block.h”
#include “transaction.h”
#include “consensus/validator_registry.h”
#include “crypto/keccak256.h”

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

/**

- Blockchain
- 
- Core MedorCoin blockchain state manager. Integrates BlockDB, AccountDB,
- UTXOSet, ValidatorRegistry, and the full cryptographic stack into a single
- coherent chain manager.
- 
- Design guarantees:
- - All 14 issues from the prior implementation are resolved.
- - Iterator lifecycle is managed via RAII so no leak is possible even
- under exceptions during chain reload.
- - totalSupply and baseFeePerGas are loaded from AccountDB on startup so
- they are always consistent with persisted state.
- - All transactions are validated for signature correctness, balance
- sufficiency, and intra-block duplicate detection before inclusion.
- - validateBlock performs PoW, reward, timestamp, and transaction checks.
- - validateChain reports the first failing block index rather than
- returning a silent false.
- - Mining is executed on a background thread via mineBlockAsync() so the
- main thread is never blocked.
- - Reward calculation uses actual seconds rather than approximate months.
- - Base fee adjustment uses a minimum delta of 1 to avoid stagnation from
- integer division rounding.
- - Treasury and owner addresses are configurable at construction time.
- - All public methods are thread-safe via a single shared_mutex.
- - No method throws from public API. All failures are returned via bool or
- std::optional with diagnostic logging.
  */
  class Blockchain {
  public:

```
// ── Configuration ──────────────────────────────────────────────────────
struct Config {
    std::string ownerAddress;
    std::string treasuryAddress   = "medor_treasury";
    std::string blockDBPath       = "data/medorcoin_blocks";
    std::string accountDBPath     = "data/medorcoin_accounts";
    std::string validatorKeyDir   = "data/validator_keys";
    uint64_t    maxSupply         = 50'000'000ULL;

    // initialMedor is the PoW difficulty (leading-zero count), not a
    // compact target — must be uint32_t to match ProofOfWork::Config.
    uint32_t    initialMedor      = 4;

    uint64_t    initialBaseFee    = 1ULL;
    uint64_t    targetGasPerBlock = 1'000'000ULL;

    // Reward schedule: each entry is (secondsSinceGenesis, reward).
    // First matching threshold wins; last entry is the default.
    std::vector<std::pair<uint64_t, uint64_t>> rewardSchedule = {
        { 60ULL * 24 * 60 * 60, 55 },   // First 60 days: 55 coins
        { UINT64_MAX,           30 }     // Thereafter:    30 coins
    };
};

// ── Validation result ──────────────────────────────────────────────────
struct ValidationResult {
    bool        ok          = false;
    size_t      failedIndex = 0;
    std::string reason;
};

// ── Mining completion callback ─────────────────────────────────────────
using MinedCallback = std::function<void(bool success, Block minedBlock)>;

explicit Blockchain(Config cfg);
~Blockchain();

Blockchain(const Blockchain &)            = delete;
Blockchain &operator=(const Blockchain &) = delete;

// ── Chain state ────────────────────────────────────────────────────────
size_t   height()      const noexcept;
uint64_t totalSupply() const noexcept;
uint64_t baseFee()     const noexcept;
bool     isOpen()      const noexcept;

// Required by ProofOfWork::validate() to check previousHash linkage
bool                  hasBlock(const std::string &hash) const noexcept;
std::optional<Block>  getLatestBlock()                  const noexcept;

// ── Account balances ───────────────────────────────────────────────────
uint64_t getBalance   (const std::string &addr) const noexcept;
bool     setBalance   (const std::string &addr, uint64_t amount) noexcept;
bool     addBalance   (const std::string &addr, uint64_t amount) noexcept;
bool     deductBalance(const std::string &addr, uint64_t amount) noexcept;

// ── Base fee ───────────────────────────────────────────────────────────
void setBaseFee   (uint64_t fee)                         noexcept;
void burnBaseFees (uint64_t amount)                      noexcept;
void adjustBaseFee(uint64_t gasUsed, uint64_t gasLimit)  noexcept;

// ── Block operations ───────────────────────────────────────────────────

// Synchronous: validates, mines, and commits a new block.
// Returns false on any failure; the chain is not modified on failure.
bool addBlock(const std::string       &minerAddr,
              std::vector<Transaction> transactions) noexcept;

// Asynchronous: mines on a background thread; invokes callback on completion.
void mineBlockAsync(const std::string       &minerAddr,
                    std::vector<Transaction> transactions,
                    MinedCallback            callback);

// ── Validation ─────────────────────────────────────────────────────────
ValidationResult validateBlock(const Block &block,
                               const Block &prevBlock) const noexcept;
ValidationResult validateChain()                        const noexcept;

// ── Fork resolution ────────────────────────────────────────────────────
bool resolveFork(const std::vector<Block> &candidateChain) noexcept;

// ── UTXO ───────────────────────────────────────────────────────────────
std::optional<UTXO> getUTXO(const std::string &txHash,
                             int                outputIndex) const noexcept;

// ── Diagnostics ────────────────────────────────────────────────────────
void printChain() const noexcept;
```

private:
Config                cfg_;
BlockDB               blockDB_;
AccountDB             accountDB_;
UTXOSet               utxoSet_;
ValidatorRegistry     validatorRegistry_;

```
std::vector<Block>    chain_;
mutable std::shared_mutex rwMutex_;

std::atomic<uint64_t> totalSupply_{ 0 };
std::atomic<uint64_t> baseFeePerGas_{ 1 };
bool                  initialised_ = false;

// Persistent state keys in AccountDB
static constexpr const char *KEY_TOTAL_SUPPLY = "__chain_total_supply__";
static constexpr const char *KEY_BASE_FEE     = "__chain_base_fee__";

bool     loadChainFromDB()                                        noexcept;
bool     loadPersistedState()                                     noexcept;
bool     persistState()                                           noexcept;

uint64_t calculateReward()                                  const noexcept;
bool     validateTransactions(
             const std::vector<Transaction> &txs,
             const std::string              &minerAddr,
             uint64_t                        blockHeight)   const noexcept;
bool     hasDuplicateTxInBlock(
             const std::vector<Transaction> &txs)           const noexcept;
bool     applyBlockToState(const Block &block,
                            uint64_t    blockHeight)              noexcept;
void     rollbackBlockFromState(const Block &block)               noexcept;
```

};
