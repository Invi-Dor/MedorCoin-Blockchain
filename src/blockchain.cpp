#include "blockchain.h"
#include "blockchain_fork.h"
#include "proof_of_work.h"
#include "crypto/keccak256.h"
#include "crypto/verify_signature.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_set>

// =============================================================================
// NETWORK FEE
// Transactions up to 1000 MEDOR: flat fee of 0.009 MEDOR (9 base units)
// Transactions above 1000 MEDOR: 2% of the output value
// Fee is deducted from recipient and credited to cfg_.treasuryAddress
// Coinbase transactions are exempt from network fee
// =============================================================================
static constexpr uint64_t NETWORK_FEE_THRESHOLD  = 1000;
static constexpr uint64_t NETWORK_FEE_FLAT       = 9;
static constexpr uint64_t NETWORK_FEE_PERCENT    = 2;

static uint64_t calculateNetworkFee(uint64_t value) noexcept
{
    if (value <= NETWORK_FEE_THRESHOLD)
        return NETWORK_FEE_FLAT;
    return (value * NETWORK_FEE_PERCENT) / 100;
}

Blockchain::Blockchain(Config cfg)
    : cfg_(std::move(cfg))
    , accountDB_(cfg_.accountDBPath)
    , utxoSet_()
{
    if (cfg_.ownerAddress.empty()) {
        std::cerr << "[Blockchain] FATAL: ownerAddress must not be empty\n";
        return;
    }
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
    loadPersistedState();
    if (!loadChainFromDB())
        std::cerr << "[Blockchain] WARNING: Chain reload encountered errors\n";
    if (chain_.empty()) {
        std::vector<Transaction> empty;
        if (!addBlock(cfg_.ownerAddress, empty)) {
            std::cerr << "[Blockchain] FATAL: Genesis block creation failed\n";
            return;
        }
    }
    initialised_ = true;
    std::cout << "[Blockchain] Initialised -- height=" << chain_.size() - 1
              << " supply=" << totalSupply_.load()
              << " baseFee=" << baseFeePerGas_.load() << "\n";
}

Blockchain::~Blockchain()
{
    persistState();
}

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
            std::cerr << "[Blockchain] loadChainFromDB: failed to read '"
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
    std::sort(chain_.begin(), chain_.end(),
              [](const Block &a, const Block &b) {
                  return a.timestamp < b.timestamp;
              });
    std::cout << "[Blockchain] Loaded " << loaded << " block(s) from DB";
    if (failed > 0) std::cout << " (" << failed << " read failure(s))";
    std::cout << "\n";
    return failed == 0;
}

bool Blockchain::loadPersistedState() noexcept
{
    std::string val;
    auto r1 = accountDB_.get(KEY_TOTAL_SUPPLY, val);
    if (r1) {
        try { totalSupply_.store(std::stoull(val)); }
        catch (...) { totalSupply_.store(0); }
    }
    auto r2 = accountDB_.get(KEY_BASE_FEE, val);
    if (r2) {
        try { baseFeePerGas_.store(std::stoull(val)); }
        catch (...) { baseFeePerGas_.store(1); }
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
    if (!r1) {
        std::cerr << "[Blockchain] persistState: totalSupply failed: "
                  << r1.error << "\n";
        ok = false;
    }
    if (!r2) {
        std::cerr << "[Blockchain] persistState: baseFee failed: "
                  << r2.error << "\n";
        ok = false;
    }
    return ok;
}

size_t Blockchain::height() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    return chain_.empty() ? 0 : chain_.size() - 1;
}

uint64_t Blockchain::totalSupply() const noexcept
{
    return totalSupply_.load();
}

uint64_t Blockchain::baseFee() const noexcept
{
    return baseFeePerGas_.load();
}

bool Blockchain::isOpen() const noexcept
{
    return initialised_;
}

bool Blockchain::hasBlock(const std::string &hash) const noexcept
{
    if (hash.empty()) return false;
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    for (const auto &b : chain_)
        if (b.hash == hash) return true;
    return false;
}

std::optional<Block> Blockchain::getLatestBlock() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    if (chain_.empty()) return std::nullopt;
    return chain_.back().clone();
}

uint64_t Blockchain::getBalance(const std::string &addr) const noexcept
{
    if (addr.empty()) return 0;
    std::string val;
    auto r = accountDB_.get("bal:" + addr, val);
    if (!r) return 0;
    try { return std::stoull(val); }
    catch (...) { return 0; }
}

bool Blockchain::setBalance(const std::string &addr,
                             uint64_t           amount) noexcept
{
    if (addr.empty()) return false;
    auto r = accountDB_.put("bal:" + addr, std::to_string(amount));
    if (!r)
        std::cerr << "[Blockchain] setBalance failed for "
                  << addr << ": " << r.error << "\n";
    return static_cast<bool>(r);
}

bool Blockchain::addBalance(const std::string &addr,
                             uint64_t           amount) noexcept
{
    if (addr.empty()) return false;
    const uint64_t current = getBalance(addr);
    if (amount > std::numeric_limits<uint64_t>::max() - current) {
        std::cerr << "[Blockchain] addBalance: overflow for " << addr << "\n";
        return false;
    }
    return setBalance(addr, current + amount);
}

bool Blockchain::deductBalance(const std::string &addr,
                                uint64_t           amount) noexcept
{
    if (addr.empty()) return false;
    const uint64_t current = getBalance(addr);
    if (amount > current) {
        std::cerr << "[Blockchain] deductBalance: insufficient for " << addr
                  << " (has " << current << ", needs " << amount << ")\n";
        return false;
    }
    return setBalance(addr, current - amount);
}

void Blockchain::setBaseFee(uint64_t fee) noexcept
{
    baseFeePerGas_.store(fee < 1 ? 1 : fee);
}

void Blockchain::burnBaseFees(uint64_t amount) noexcept
{
    if (!addBalance(cfg_.treasuryAddress, amount))
        std::cerr << "[Blockchain] burnBaseFees: failed to credit treasury\n";
}

void Blockchain::adjustBaseFee(uint64_t gasUsed,
                                uint64_t gasLimit) noexcept
{
    if (gasLimit == 0) return;
    const uint64_t current = baseFeePerGas_.load();
    uint64_t next = current;
    if (gasUsed * 2 > gasLimit) {
        const uint64_t delta = std::max(uint64_t{1}, current / 8);
        next = current + delta;
    } else {
        const uint64_t delta = std::max(uint64_t{1}, current / 8);
        next = (current > delta) ? current - delta : 1;
    }
    baseFeePerGas_.store(std::max(next, uint64_t{1}));
}

uint64_t Blockchain::calculateReward() const noexcept
{
    if (chain_.empty())
        return cfg_.rewardSchedule.empty()
               ? 0 : cfg_.rewardSchedule.front().second;
    const uint64_t genesisTime =
        static_cast<uint64_t>(chain_.front().timestamp);
    const uint64_t now =
        static_cast<uint64_t>(std::time(nullptr));
    const uint64_t elapsed =
        (now >= genesisTime) ? (now - genesisTime) : 0;
    for (const auto &[threshold, reward] : cfg_.rewardSchedule)
        if (elapsed < threshold) return reward;
    return cfg_.rewardSchedule.back().second;
}

bool Blockchain::hasDuplicateTxInBlock(
    const std::vector<Transaction> &txs) const noexcept
{
    std::unordered_set<std::string> seen;
    for (const auto &tx : txs) {
        if (tx.txHash.empty()) continue;
        if (!seen.insert(tx.txHash).second) {
            std::cerr << "[Blockchain] hasDuplicateTxInBlock: duplicate "
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

    std::unordered_set<std::string> spentThisBlock;

    for (size_t i = 0; i < txs.size(); ++i) {
        const Transaction &tx = txs[i];
        if (i == 0) {
            uint64_t cbValue = 0;
            for (const auto &out : tx.outputs) cbValue += out.value;
            const uint64_t expected  = calculateReward();
            const uint64_t remaining = cfg_.maxSupply - totalSupply_.load();
            const uint64_t allowed   = std::min(expected, remaining);
            if (cbValue > allowed) {
                std::cerr << "[Blockchain] validateTransactions: coinbase claims "
                          << cbValue << " max allowed " << allowed << "\n";
                return false;
            }
            continue;
        }
        if (tx.txHash.empty()) {
            std::cerr << "[Blockchain] validateTransactions: tx[" << i
                      << "] empty txHash\n";
            return false;
        }
        if (!tx.isValid()) {
            std::cerr << "[Blockchain] validateTransactions: tx[" << i
                      << "] failed isValid() hash=" << tx.txHash << "\n";
            return false;
        }
        uint64_t inputSum = 0;
        for (const auto &in : tx.inputs) {
            const std::string outpointKey =
                in.prevTxHash + ":" + std::to_string(in.outputIndex);
            if (!spentThisBlock.insert(outpointKey).second) {
                std::cerr << "[Blockchain] validateTransactions: double-spend on "
                          << outpointKey << "\n";
                return false;
            }
            auto utxo = utxoSet_.getUTXO(in.prevTxHash, in.outputIndex);
            if (!utxo) {
                std::cerr << "[Blockchain] validateTransactions: UTXO "
                          << outpointKey << " not found\n";
                return false;
            }
            if (tx.r != std::array<uint8_t, 32>{}
             || tx.s != std::array<uint8_t, 32>{}) {
                crypto::Keccak256Digest signingHash{};
                if (!crypto::Keccak256(
                        reinterpret_cast<const uint8_t *>(tx.txHash.data()),
                        tx.txHash.size(), signingHash)) {
                    std::cerr << "[Blockchain] validateTransactions: "
                                 "Keccak256 failed\n";
                    return false;
                }
                std::array<uint8_t, 20> expectedAddr{};
                const std::string &addrHex = utxo->address;
                if (addrHex.size() == 40) {
                    for (size_t j = 0; j < 20; ++j) {
                        unsigned int byte = 0;
                        std::istringstream ss(addrHex.substr(j * 2, 2));
                        ss >> std::hex >> byte;
                        expectedAddr[j] = static_cast<uint8_t>(byte);
                    }
                }
                if (!crypto::recoverAndVerify(
                        signingHash.data(),
                        tx.r.data(), tx.s.data(),
                        static_cast<int>(tx.v),
                        expectedAddr.data())) {
                    std::cerr << "[Blockchain] validateTransactions: "
                                 "sig failed for " << tx.txHash << "\n";
                    return false;
                }
            }
            inputSum += utxo->value;
        }
        uint64_t outputSum = 0;
        for (const auto &out : tx.outputs) outputSum += out.value;
        if (outputSum > inputSum) {
            std::cerr << "[Blockchain] validateTransactions: "
                         "outputs exceed inputs for " << tx.txHash << "\n";
            return false;
        }
    }
    return true;
}

bool Blockchain::applyBlockToState(const Block  &block,
                                    uint64_t      blockHeight) noexcept
{
    for (size_t txIdx = 0; txIdx < block.transactions.size(); ++txIdx) {
        const Transaction &tx = block.transactions[txIdx];
        const bool isCoinbase = (txIdx == 0);

        for (const auto &in : tx.inputs) {
            if (!utxoSet_.spendUTXO(in.prevTxHash,
                                     in.outputIndex, blockHeight)) {
                std::cerr << "[Blockchain] applyBlockToState: spendUTXO failed "
                          << in.prevTxHash << ":" << in.outputIndex << "\n";
                return false;
            }
        }

        for (size_t i = 0; i < tx.outputs.size(); ++i) {
            if (!utxoSet_.addUTXO(tx.outputs[i], tx.txHash,
                                   static_cast<int>(i),
                                   blockHeight, isCoinbase)) {
                std::cerr << "[Blockchain] applyBlockToState: addUTXO failed "
                          << tx.txHash << ":" << i << "\n";
                return false;
            }

            // Credit recipient balance
            addBalance(tx.outputs[i].address, tx.outputs[i].value);

            // =================================================================
            // NETWORK FEE
            // Exempt: coinbase transactions (block rewards)
            // Up to 1000 MEDOR: flat 0.009 MEDOR (9 base units)
            // Above 1000 MEDOR: 2% of output value
            // Deducted from recipient, credited to treasury
            // =================================================================
            if (!isCoinbase && !cfg_.treasuryAddress.empty()) {
                uint64_t fee = calculateNetworkFee(tx.outputs[i].value);
                // Clamp fee so it never exceeds the output value
                fee = std::min(fee, tx.outputs[i].value);
                if (fee > 0) {
                    if (deductBalance(tx.outputs[i].address, fee)) {
                        addBalance(cfg_.treasuryAddress, fee);
                        std::cout << "[Blockchain] Network fee "
                                  << fee
                                  << " collected from "
                                  << tx.outputs[i].address
                                  << " to treasury\n";
                    }
                }
            }
        }
    }
    return true;
}

void Blockchain::rollbackBlockFromState(const Block &block) noexcept
{
    for (auto txIt = block.transactions.rbegin();
         txIt != block.transactions.rend(); ++txIt)
    {
        for (size_t i = 0; i < txIt->outputs.size(); ++i)
            (void)utxoSet_.spendUTXO(txIt->txHash, static_cast<int>(i),
                               std::numeric_limits<uint64_t>::max());
    }
}

bool Blockchain::addBlock(const std::string       &minerAddr,
                           std::vector<Transaction> transactions) noexcept
{
    if (minerAddr.empty()) {
        std::cerr << "[Blockchain] addBlock: empty minerAddr rejected\n";
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    const uint64_t blockHeight = static_cast<uint64_t>(chain_.size());
    uint64_t reward            = calculateReward();
    const uint64_t remaining   = cfg_.maxSupply - totalSupply_.load();
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
            std::cerr << "[Blockchain] addBlock: coinbase hash failed\n";
            return false;
        }
        transactions.insert(transactions.begin(), coinbaseTx);
    }
    if (!validateTransactions(transactions, minerAddr, blockHeight)) {
        std::cerr << "[Blockchain] addBlock: transaction validation failed\n";
        return false;
    }
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
    {
        ProofOfWork::Config powCfg;
        powCfg.minDifficulty = cfg_.initialMedor;
        powCfg.maxDifficulty = cfg_.initialMedor;
        ProofOfWork pow(std::move(powCfg));
        std::atomic<bool> abort{false};
        auto result = pow.mine(newBlock, abort);
        if (!result.found) {
            std::cerr << "[Blockchain] addBlock: PoW mining failed\n";
            return false;
        }
    }
    auto writeResult = blockDB_.writeBlock(newBlock);
    if (!writeResult) {
        std::cerr << "[Blockchain] addBlock: BlockDB write failed: "
                  << writeResult.error << "\n";
        return false;
    }
        if (!applyBlockToState(newBlock, blockHeight)) {
        rollbackBlockFromState(newBlock);
        std::cerr << "[Blockchain] addBlock: state application failed "
                     "-- rolling back\n";
        return false;
    }
    chain_.push_back(std::move(newBlock));
    totalSupply_.fetch_add(reward);
    adjustBaseFee(newBlock.gasUsed, cfg_.targetGasPerBlock);
    persistState();
    std::cout << "[Blockchain] Block " << blockHeight
              << " committed | hash=" << chain_.back().hash
              << " | reward=" << reward
              << " | baseFee=" << baseFeePerGas_.load() << "\n";
    return true;
}

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
