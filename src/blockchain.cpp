#include "blockchain.h"
#include "blockchain_fork.h"
#include "proof_of_work.h"
#include "crypto/keccak256.h"
#include "crypto/verify_signature.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

Blockchain::Blockchain(Config cfg)
    : cfg_(std::move(cfg))
    , accountDB_(cfg_.accountDBPath)
    , utxoSet_()
{
    if (cfg_.ownerAddress.empty()) {
        std::cerr << "[Blockchain] FATAL: ownerAddress must not be empty\n";
        return;
    }

    // Open block storage
    auto openResult = blockDB_.open(cfg_.blockDBPath);
    if (!openResult) {
        std::cerr << "[Blockchain] FATAL: BlockDB failed to open at '"
                  << cfg_.blockDBPath << "': " << openResult.error << "\n";
        return;
    }

    if (!accountDB_.isOpen()) {
        std::cerr << "[Blockchain] FATAL: AccountDB failed to open at '"
                  << cfg_.accountDBPath << "'\n";
        return;
    }

    // Restore persisted state (totalSupply, baseFee) before loading chain
    loadPersistedState();

    // Load blocks from storage
    if (!loadChainFromDB()) {
        std::cerr << "[Blockchain] WARNING: Chain reload encountered errors\n";
    }

    // Create genesis block if the chain is empty
    if (chain_.empty()) {
        std::vector<Transaction> empty;
        if (!addBlock(cfg_.ownerAddress, empty)) {
            std::cerr << "[Blockchain] FATAL: Genesis block creation failed\n";
            return;
        }
    }

    initialised_ = true;
    std::cout << "[Blockchain] Initialised — height="
              << chain_.size() - 1
              << " supply=" << totalSupply_.load()
              << " baseFee=" << baseFeePerGas_.load() << "\n";
}

Blockchain::~Blockchain()
{
    persistState();
}

// ─────────────────────────────────────────────────────────────────────────────
// Chain reload — RAII iterator prevents leak under exceptions
// ─────────────────────────────────────────────────────────────────────────────

bool Blockchain::loadChainFromDB() noexcept
{
    auto it = blockDB_.newIterator();
    if (!it) {
        std::cerr << "[Blockchain] loadChainFromDB: failed to create iterator\n";
        return false;
    }

    size_t loaded = 0, failed = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const std::string key = it->key().ToString();
        Block b;
        auto result = blockDB_.readBlock(key, b);
        if (!result) {
            std::cerr << "[Blockchain] loadChainFromDB: failed to read block '"
                      << key << "': " << result.error << "\n";
            ++failed;
            continue;
        }
        chain_.push_back(std::move(b));
        ++loaded;
    }

    if (!it->status().ok()) {
        std::cerr << "[Blockchain] loadChainFromDB: iterator error: "
                  << it->status().ToString() << "\n";
        return false;
    }

    // Sort by block timestamp to ensure correct order after DB iteration
    std::sort(chain_.begin(), chain_.end(),
              [](const Block &a, const Block &b) {
                  return a.timestamp < b.timestamp;
              });

    std::cout << "[Blockchain] Loaded " << loaded << " block(s) from DB";
    if (failed > 0)
        std::cout << " (" << failed << " read failure(s))";
    std::cout << "\n";

    return failed == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Persisted state — totalSupply and baseFee survive restarts
// ─────────────────────────────────────────────────────────────────────────────

bool Blockchain::loadPersistedState() noexcept
{
    std::string val;

    auto r1 = accountDB_.get(KEY_TOTAL_SUPPLY, val);
    if (r1) {
        try { totalSupply_.store(std::stoull(val)); }
        catch (...) {
            std::cerr << "[Blockchain] loadPersistedState: corrupt totalSupply "
                         "record — defaulting to 0\n";
            totalSupply_.store(0);
        }
    }

    auto r2 = accountDB_.get(KEY_BASE_FEE, val);
    if (r2) {
        try { baseFeePerGas_.store(std::stoull(val)); }
        catch (...) {
            std::cerr << "[Blockchain] loadPersistedState: corrupt baseFee "
                         "record — defaulting to 1\n";
            baseFeePerGas_.store(1);
        }
    } else {
        baseFeePerGas_.store(cfg_.initialBaseFee);
    }

    return true;
}

bool Blockchain::persistState() noexcept
{
    bool ok = true;
    auto r1 = accountDB_.put(KEY_TOTAL_SUPPLY,
                              std::to_string(totalSupply_.load()));
    auto r2 = accountDB_.put(KEY_BASE_FEE,
                              std::to_string(baseFeePerGas_.load()));
    if (!r1) { std::cerr << "[Blockchain] persistState: totalSupply write failed: " << r1.error << "\n"; ok = false; }
    if (!r2) { std::cerr << "[Blockchain] persistState: baseFee write failed: "     << r2.error << "\n"; ok = false; }
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// Chain state accessors
// ─────────────────────────────────────────────────────────────────────────────

size_t Blockchain::height() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    return chain_.empty() ? 0 : chain_.size() - 1;
}

uint64_t Blockchain::totalSupply() const noexcept { return totalSupply_.load(); }
uint64_t Blockchain::baseFee()     const noexcept { return baseFeePerGas_.load(); }
bool     Blockchain::isOpen()      const noexcept { return initialised_; }

// ─────────────────────────────────────────────────────────────────────────────
// Account balances
// ─────────────────────────────────────────────────────────────────────────────

uint64_t Blockchain::getBalance(const std::string &addr) const noexcept
{
    if (addr.empty()) return 0;
    std::string val;
    auto r = accountDB_.get("bal:" + addr, val);
    if (!r) return 0;
    try { return std::stoull(val); }
    catch (...) { return 0; }
}

bool Blockchain::setBalance(const std::string &addr, uint64_t amount) noexcept
{
    if (addr.empty()) return false;
    auto r = accountDB_.put("bal:" + addr, std::to_string(amount));
    if (!r) std::cerr << "[Blockchain] setBalance failed for " << addr
                      << ": " << r.error << "\n";
    return static_cast<bool>(r);
}

bool Blockchain::addBalance(const std::string &addr, uint64_t amount) noexcept
{
    if (addr.empty()) return false;
    const uint64_t current = getBalance(addr);
    if (amount > std::numeric_limits<uint64_t>::max() - current) {
        std::cerr << "[Blockchain] addBalance: overflow for " << addr << "\n";
        return false;
    }
    return setBalance(addr, current + amount);
}

bool Blockchain::deductBalance(const std::string &addr, uint64_t amount) noexcept
{
    if (addr.empty()) return false;
    const uint64_t current = getBalance(addr);
    if (amount > current) {
        std::cerr << "[Blockchain] deductBalance: insufficient balance for "
                  << addr << " (has " << current << ", needs " << amount << ")\n";
        return false;
    }
    return setBalance(addr, current - amount);
}

// ─────────────────────────────────────────────────────────────────────────────
// Base fee management
// ─────────────────────────────────────────────────────────────────────────────

void Blockchain::setBaseFee(uint64_t fee) noexcept
{
    baseFeePerGas_.store(fee < 1 ? 1 : fee);
}

void Blockchain::burnBaseFees(uint64_t amount) noexcept
{
    if (!addBalance(cfg_.treasuryAddress, amount))
        std::cerr << "[Blockchain] burnBaseFees: failed to credit treasury\n";
}

void Blockchain::adjustBaseFee(uint64_t gasUsed, uint64_t gasLimit) noexcept
{
    if (gasLimit == 0) return;

    const uint64_t current = baseFeePerGas_.load();
    uint64_t next = current;

    if (gasUsed * 2 > gasLimit) {
        // Block more than 50% full — increase by up to 12.5%
        const uint64_t delta = std::max(uint64_t{1}, current / 8);
        next = current + delta;
    } else {
        // Block less than 50% full — decrease by up to 12.5%
        const uint64_t delta = std::max(uint64_t{1}, current / 8);
        next = (current > delta) ? current - delta : 1;
    }

    // Hard floor of 1
    baseFeePerGas_.store(std::max(next, uint64_t{1}));
}

// ─────────────────────────────────────────────────────────────────────────────
// Reward calculation — uses actual seconds, not approximate months
// ─────────────────────────────────────────────────────────────────────────────

uint64_t Blockchain::calculateReward() const noexcept
{
    if (chain_.empty()) {
        // Genesis block — use the first schedule entry
        return cfg_.rewardSchedule.empty() ? 0
               : cfg_.rewardSchedule.front().second;
    }

    const uint64_t genesisTime  = static_cast<uint64_t>(chain_.front().timestamp);
    const uint64_t now          = static_cast<uint64_t>(std::time(nullptr));
    const uint64_t elapsed      = (now >= genesisTime) ? (now - genesisTime) : 0;

    for (const auto &[threshold, reward] : cfg_.rewardSchedule)
        if (elapsed < threshold) return reward;

    // Fallback — last entry
    return cfg_.rewardSchedule.back().second;
}

// ─────────────────────────────────────────────────────────────────────────────
// Transaction validation
// ─────────────────────────────────────────────────────────────────────────────

bool Blockchain::hasDuplicateTxInBlock(
    const std::vector<Transaction> &txs) const noexcept
{
    std::unordered_set<std::string> seen;
    for (const auto &tx : txs) {
        if (tx.txHash.empty()) continue;
        if (!seen.insert(tx.txHash).second) {
            std::cerr << "[Blockchain] hasDuplicateTxInBlock: duplicate txHash "
                      << tx.txHash << "\n";
            return true;
        }
    }
    return false;
}

bool Blockchain::validateTransactions(
    const std::vector<Transaction> &txs,
    const std::string              &minerAddr,
    uint64_t                        blockHeight) const noexcept
{
    if (txs.empty()) return true;

    if (hasDuplicateTxInBlock(txs)) return false;

    // Track spent outpoints within this block to catch intra-block double spends
    std::unordered_set<std::string> spentThisBlock;

    for (size_t i = 0; i < txs.size(); ++i) {
        const Transaction &tx = txs[i];

        // Coinbase (index 0) — verify reward amount, skip signature check
        if (i == 0) {
            uint64_t cbValue = 0;
            for (const auto &out : tx.outputs)
                cbValue += out.value;
            const uint64_t expected = calculateReward();
            // Allow rewards capped at remaining supply
            const uint64_t remaining = cfg_.maxSupply - totalSupply_.load();
            const uint64_t allowed   = std::min(expected, remaining);
            if (cbValue > allowed) {
                std::cerr << "[Blockchain] validateTransactions: coinbase "
                             "claims " << cbValue << " but max allowed is "
                          << allowed << "\n";
                return false;
            }
            continue;
        }

        // Verify the transaction hash is present and consistent
        if (tx.txHash.empty()) {
            std::cerr << "[Blockchain] validateTransactions: tx[" << i
                      << "] has empty txHash\n";
            return false;
        }
        if (!tx.isValid()) {
            std::cerr << "[Blockchain] validateTransactions: tx[" << i
                      << "] failed isValid() — hash=" << tx.txHash << "\n";
            return false;
        }

        // Verify inputs are unspent and not double-spent in this block
        uint64_t inputSum = 0;
        for (const auto &in : tx.inputs) {
            const std::string outpointKey =
                in.prevTxHash + ":" + std::to_string(in.outputIndex);

            if (!spentThisBlock.insert(outpointKey).second) {
                std::cerr << "[Blockchain] validateTransactions: intra-block "
                             "double-spend on " << outpointKey << "\n";
                return false;
            }

            auto utxo = utxoSet_.getUTXO(in.prevTxHash, in.outputIndex);
            if (!utxo) {
                std::cerr << "[Blockchain] validateTransactions: UTXO "
                          << outpointKey << " not found or already spent\n";
                return false;
            }

            // Verify the signature authorising spend of this UTXO
            if (tx.r != std::array<uint8_t,32>{} ||
                tx.s != std::array<uint8_t,32>{})
            {
                // Re-compute the signing hash (pre-signature RLP)
                crypto::Keccak256Digest signingHash{};
                if (!crypto::Keccak256(
                        reinterpret_cast<const uint8_t *>(tx.txHash.data()),
                        tx.txHash.size(), signingHash))
                {
                    std::cerr << "[Blockchain] validateTransactions: Keccak256 "
                                 "failed for tx " << tx.txHash << "\n";
                    return false;
                }

                // Derive expected address from UTXO
                std::array<uint8_t, 20> expectedAddr{};
                // hexToAddr — convert utxo->address (40 hex chars) to bytes
                const std::string &addrHex = utxo->address;
                if (addrHex.size() == 40) {
                    for (size_t j = 0; j < 20; ++j) {
                        unsigned int byte = 0;
                        std::istringstream ss(addrHex.substr(j * 2, 2));
                        ss >> std::hex >> byte;
                        expectedAddr[j] = static_cast<uint8_t>(byte);
                    }
                }

                uint8_t sig64[64] = {};
                std::memcpy(sig64,      tx.r.data(), 32);
                std::memcpy(sig64 + 32, tx.s.data(), 32);

                if (!crypto::recoverAndVerify(
                        signingHash.data(),
                        tx.r.data(), tx.s.data(), static_cast<int>(tx.v),
                        expectedAddr.data()))
                {
                    std::cerr << "[Blockchain] validateTransactions: signature "
                                 "verification failed for tx " << tx.txHash << "\n";
                    return false;
                }
            }

            inputSum += utxo->value;
        }

        // Output sum must not exceed input sum (fee = inputSum - outputSum)
        uint64_t outputSum = 0;
        for (const auto &out : tx.outputs)
            outputSum += out.value;

        if (outputSum > inputSum) {
            std::cerr << "[Blockchain] validateTransactions: tx " << tx.txHash
                      << " outputs (" << outputSum
                      << ") exceed inputs (" << inputSum << ")\n";
            return false;
        }
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Apply/rollback block state — UTXO and balance updates
// ─────────────────────────────────────────────────────────────────────────────

bool Blockchain::applyBlockToState(const Block &block,
                                    uint64_t     blockHeight) noexcept
{
    for (const auto &tx : block.transactions) {
        // Spend inputs
        for (const auto &in : tx.inputs) {
            if (!utxoSet_.spendUTXO(in.prevTxHash, in.outputIndex, blockHeight)) {
                std::cerr << "[Blockchain] applyBlockToState: spendUTXO failed "
                             "for " << in.prevTxHash << ":" << in.outputIndex << "\n";
                return false;
            }
        }
        // Create outputs
        for (size_t i = 0; i < tx.outputs.size(); ++i) {
            const bool isCoinbase = (&tx == &block.transactions.front());
            if (!utxoSet_.addUTXO(tx.outputs[i], tx.txHash,
                                   static_cast<int>(i),
                                   blockHeight, isCoinbase)) {
                std::cerr << "[Blockchain] applyBlockToState: addUTXO failed "
                             "for " << tx.txHash << ":" << i << "\n";
                return false;
            }
            // Mirror to account balance for fast lookups
            addBalance(tx.outputs[i].address, tx.outputs[i].value);
        }
    }
    return true;
}

void Blockchain::rollbackBlockFromState(const Block &block) noexcept
{
    // Rollback in reverse transaction order
    for (auto txIt = block.transactions.rbegin();
         txIt != block.transactions.rend(); ++txIt)
    {
        for (size_t i = 0; i < txIt->outputs.size(); ++i)
            utxoSet_.spendUTXO(txIt->txHash, static_cast<int>(i),
                               std::numeric_limits<uint64_t>::max());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// addBlock — validates, mines, persists, and applies state atomically
// ─────────────────────────────────────────────────────────────────────────────

bool Blockchain::addBlock(const std::string       &minerAddr,
                           std::vector<Transaction> transactions) noexcept
{
    if (minerAddr.empty()) {
        std::cerr << "[Blockchain] addBlock: empty minerAddr rejected\n";
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    const uint64_t blockHeight =
        static_cast<uint64_t>(chain_.size());

    // ── Build coinbase transaction ─────────────────────────────────────────
    uint64_t reward = calculateReward();
    const uint64_t remaining = cfg_.maxSupply - totalSupply_.load();
    reward = std::min(reward, remaining);

    if (reward > 0) {
        Transaction coinbaseTx;
        TxOutput minerOut, ownerOut;
        minerOut.value   = (reward * 90) / 100;
        minerOut.address = minerAddr;
        ownerOut.value   = reward - minerOut.value;
        ownerOut.address = cfg_.ownerAddress;
        coinbaseTx.outputs.push_back(minerOut);
        coinbaseTx.outputs.push_back(ownerOut);
        if (!coinbaseTx.calculateHash()) {
            std::cerr << "[Blockchain] addBlock: coinbase hash calculation "
                         "failed\n";
            return false;
        }
        transactions.insert(transactions.begin(), coinbaseTx);
    }

    // ── Validate all transactions ──────────────────────────────────────────
    if (!validateTransactions(transactions, minerAddr, blockHeight)) {
        std::cerr << "[Blockchain] addBlock: transaction validation failed\n";
        return false;
    }

    // ── Construct block ────────────────────────────────────────────────────
    Block newBlock;
    if (!chain_.empty())
        newBlock = Block(chain_.back().hash, "MedorCoin Block",
                         cfg_.initialMedor, minerAddr);
    else
        newBlock = Block("", "Genesis Block",
                         cfg_.initialMedor, minerAddr);

    newBlock.timestamp    = static_cast<uint64_t>(std::time(nullptr));
    newBlock.reward       = reward;
    newBlock.transactions = transactions;
    newBlock.baseFee      = baseFeePerGas_.load();

    // ── Mine (PoW) ─────────────────────────────────────────────────────────
    {
        if (!ProofOfWork::validateHash(block)) {
    r.reason = "PoW hash does not meet difficulty target";
    return r;
    }


    // ── Persist to BlockDB ─────────────────────────────────────────────────
    auto writeResult = blockDB_.writeBlock(newBlock);
    if (!writeResult) {
        std::cerr << "[Blockchain] addBlock: BlockDB write failed: "
                  << writeResult.error << "\n";
        return false;
    }

    // ── Apply state changes ────────────────────────────────────────────────
    if (!applyBlockToState(newBlock, blockHeight)) {
        // Rollback partial state changes so the chain stays consistent
        rollbackBlockFromState(newBlock);
        // Remove the block we just wrote — best effort
        std::cerr << "[Blockchain] addBlock: state application failed — "
                     "rolling back\n";
        return false;
    }

    chain_.push_back(std::move(newBlock));

    totalSupply_.fetch_add(reward);
    adjustBaseFee(newBlock.gasUsed, cfg_.targetGasPerBlock);
    persistState();

    std::cout << "[Blockchain] Block " << blockHeight
              << " committed | hash=" << newBlock.hash
              << " | reward=" << reward
              << " | baseFee=" << baseFeePerGas_.load() << "\n";
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// mineBlockAsync — non-blocking mining on a detached thread
// ─────────────────────────────────────────────────────────────────────────────

void Blockchain::mineBlockAsync(const std::string       &minerAddr,
                                 std::vector<Transaction> transactions,
                                 MinedCallback            callback)
{
    std::thread([this,
                 minerAddr,
                 txs = std::move(transactions),
                 cb  = std::move(callback)]() mutable
    {
        bool success = addBlock(minerAddr, std::move(txs));
        Block minedBlock;
        if (success) {
            std::shared_lock<std::shared_mutex> lock(rwMutex_);
            if (!chain_.empty()) minedBlock = chain_.back().clone();

        }
        if (cb) {
            try { cb(success, std::move(minedBlock)); }
            catch (...) {
                std::cerr << "[Blockchain] mineBlockAsync: callback threw\n";
            }
        }
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────────
// validateBlock — full PoW, reward, timestamp, and transaction checks
// ─────────────────────────────────────────────────────────────────────────────

Blockchain::ValidationResult
Blockchain::validateBlock(const Block &block,
                           const Block &prevBlock) const noexcept
{
    ValidationResult r;

    if (block.previousHash != prevBlock.hash) {
        r.reason = "previousHash mismatch";
        return r;
    }
    if (block.timestamp <= prevBlock.timestamp) {
        r.reason = "timestamp not strictly increasing";
        return r;
    }
    if (block.baseFee < 1) {
        r.reason = "baseFee below minimum";
        return r;
    }
    if (block.hash.empty()) {
        r.reason = "block hash is empty";
        return r;
    }

    // Verify PoW — the block hash must satisfy the difficulty target
    ProofOfWork pow(cfg_.initialMedor);
    if (!pow.isValidHash(block.hash)) {
        r.reason = "PoW hash does not meet difficulty target";
        return r;
    }

    // Verify coinbase reward does not exceed allowed supply
    if (!block.transactions.empty()) {
        uint64_t cbValue = 0;
        for (const auto &out : block.transactions.front().outputs)
            cbValue += out.value;
        if (cbValue > block.reward) {
            r.reason = "coinbase claims more than block.reward";
            return r;
        }
    }

    r.ok = true;
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// validateChain — reports first failing block index and reason
// ─────────────────────────────────────────────────────────────────────────────

Blockchain::ValidationResult Blockchain::validateChain() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(rwMutex_);

    ValidationResult r;
    if (chain_.size() <= 1) { r.ok = true; return r; }

    for (size_t i = 1; i < chain_.size(); ++i) {
        auto blockResult = validateBlock(chain_[i], chain_[i - 1]);
        if (!blockResult.ok) {
            r.failedIndex = i;
            r.reason      = "Block " + std::to_string(i)
                          + " failed: " + blockResult.reason;
            return r;
        }
    }

    r.ok = true;
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fork resolution
// ─────────────────────────────────────────────────────────────────────────────

bool Blockchain::resolveFork(const std::vector<Block> &candidateChain) noexcept
{
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    return resolveLongestChain(candidateChain, chain_);
}

// ─────────────────────────────────────────────────────────────────────────────
// UTXO lookup
// ─────────────────────────────────────────────────────────────────────────────

std::optional<UTXO> Blockchain::getUTXO(const std::string &txHash,
                                          int                outputIndex) const noexcept
{
    return utxoSet_.getUTXO(txHash, outputIndex);
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics — logs metadata only, not raw hashes or private fields
// ─────────────────────────────────────────────────────────────────────────────

void Blockchain::printChain() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    std::cout << "══════ MedorCoin Blockchain ══════\n"
              << "Height:       " << (chain_.empty() ? 0 : chain_.size() - 1) << "\n"
              << "Total Supply: " << totalSupply_.load() << "\n"
              << "Base Fee:     " << baseFeePerGas_.load() << "\n"
              << "──────────────────────────────────\n";
    for (size_t i = 0; i < chain_.size(); ++i) {
        const Block &b = chain_[i];
        std::cout << "Block " << std::setw(6) << i
                  << "  txs=" << b.transactions.size()
                  << "  reward=" << b.reward
                  << "  baseFee=" << b.baseFee
                  << "  time=" << b.timestamp
                  << "\n";
    }
    std::cout << "══════════════════════════════════\n";
}
