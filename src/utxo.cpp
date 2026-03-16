#include "utxo.h"

#include <algorithm>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

UTXOSet::UTXOSet(PersistFn fn)
    : persistFn_(std::move(fn))
{
}

// ─────────────────────────────────────────────────────────────────────────────
// makeKey
//
// Format: "<hashLen>:<txHash>:<index>"
//
// The leading length prefix guarantees uniqueness even when txHash itself
// contains a colon. For example:
//   txHash="abc:def", index=1  →  "7:abc:def:1"
//   txHash="7:abc",   index=2  →  "5:7:abc:2"
// These two keys are distinct because their length prefixes differ.
// ─────────────────────────────────────────────────────────────────────────────

std::string UTXOSet::makeKey(const std::string &txHash,
                               int                outputIndex) noexcept
{
    return std::to_string(txHash.size())
         + ":"  + txHash
         + ":"  + std::to_string(outputIndex);
}

// ─────────────────────────────────────────────────────────────────────────────
// Secondary index helpers
//
// Precondition: the write lock must be held by the caller.
// These methods are private and are only invoked from addUTXO, spendUTXO,
// clear, and loadSnapshot — all of which hold the write lock — so the
// precondition is always satisfied.
// ─────────────────────────────────────────────────────────────────────────────

void UTXOSet::indexAdd(const std::string &address,
                        const std::string &key) noexcept
{
    addressIndex_[address].push_back(key);
}

void UTXOSet::indexRemove(const std::string &address,
                           const std::string &key) noexcept
{
    auto it = addressIndex_.find(address);
    if (it == addressIndex_.end()) return;

    auto &vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), key), vec.end());

    if (vec.empty()) addressIndex_.erase(it);
}

// ─────────────────────────────────────────────────────────────────────────────
// persist
//
// Called while the write lock is still held so no other thread can modify
// utxos_ between the in-memory mutation and the persistence callback.
// The lock is shared_mutex, and we hold a unique_lock at this point, so
// the snapshot the callback receives is guaranteed consistent.
// ─────────────────────────────────────────────────────────────────────────────

void UTXOSet::persist() const noexcept
{
    if (!persistFn_) return;
    try {
        persistFn_(utxos_);
    } catch (const std::exception &e) {
        std::cerr << "[UTXOSet] persist: callback threw: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[UTXOSet] persist: callback threw unknown exception\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// addUTXO
// ─────────────────────────────────────────────────────────────────────────────

bool UTXOSet::addUTXO(const TxOutput    &output,
                       const std::string &txHash,
                       int                outputIndex,
                       uint64_t           blockHeight,
                       bool               isCoinbase) noexcept
{
    if (txHash.empty()) {
        std::cerr << "[UTXOSet] addUTXO: empty txHash rejected\n";
        return false;
    }
    if (outputIndex < 0) {
        std::cerr << "[UTXOSet] addUTXO: negative outputIndex rejected\n";
        return false;
    }
    if (output.address.empty()) {
        std::cerr << "[UTXOSet] addUTXO: empty address rejected\n";
        return false;
    }

    const std::string key = makeKey(txHash, outputIndex);

    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    if (utxos_.count(key)) {
        std::cerr << "[UTXOSet] addUTXO: duplicate key '" << key
                  << "' rejected\n";
        return false;
    }

    UTXO utxo;
    utxo.txHash      = txHash;
    utxo.outputIndex = outputIndex;
    utxo.value       = output.value;
    utxo.address     = output.address;
    utxo.blockHeight = blockHeight;
    utxo.isCoinbase  = isCoinbase;

    utxos_.emplace(key, utxo);
    indexAdd(output.address, key);
    persist();   // called while write lock is held
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// spendUTXO
// ─────────────────────────────────────────────────────────────────────────────

bool UTXOSet::spendUTXO(const std::string &txHash,
                         int                outputIndex,
                         uint64_t           currentBlockHeight) noexcept
{
    if (txHash.empty()) {
        std::cerr << "[UTXOSet] spendUTXO: empty txHash rejected\n";
        return false;
    }
    if (outputIndex < 0) {
        std::cerr << "[UTXOSet] spendUTXO: negative outputIndex rejected\n";
        return false;
    }

    const std::string key = makeKey(txHash, outputIndex);

    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    auto it = utxos_.find(key);
    if (it == utxos_.end()) {
        std::cerr << "[UTXOSet] spendUTXO: key '" << key
                  << "' not found (already spent or nonexistent)\n";
        return false;
    }

    const UTXO &utxo = it->second;

    // Enforce coinbase maturity. Passing UINT64_MAX as currentBlockHeight
    // is the internal sentinel used by rollback paths to bypass this check.
    if (utxo.isCoinbase &&
        currentBlockHeight != std::numeric_limits<uint64_t>::max())
    {
        if (currentBlockHeight < utxo.blockHeight + COINBASE_MATURITY) {
            std::cerr << "[UTXOSet] spendUTXO: coinbase UTXO '" << key
                      << "' has not matured (created=" << utxo.blockHeight
                      << ", current=" << currentBlockHeight
                      << ", required=" << (utxo.blockHeight + COINBASE_MATURITY)
                      << ")\n";
            return false;
        }
    }

    indexRemove(utxo.address, key);
    utxos_.erase(it);
    persist();   // called while write lock is held
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// getBalance
//
// Returns std::nullopt on overflow rather than throwing inside a noexcept
// function. The caller can then decide whether to cap, reject, or log the
// condition — the program is never silently terminated.
// ─────────────────────────────────────────────────────────────────────────────

std::optional<uint64_t> UTXOSet::getBalance(
    const std::string &address) const noexcept
{
    if (address.empty()) return 0ULL;

    std::shared_lock<std::shared_mutex> lock(rwMutex_);

    const auto idxIt = addressIndex_.find(address);
    if (idxIt == addressIndex_.end()) return 0ULL;

    uint64_t balance = 0;
    for (const auto &key : idxIt->second) {
        const auto utxoIt = utxos_.find(key);
        if (utxoIt == utxos_.end()) continue;

        const uint64_t v = utxoIt->second.value;
        if (v > std::numeric_limits<uint64_t>::max() - balance) {
            std::cerr << "[UTXOSet] getBalance: uint64 overflow detected "
                         "for address '" << address << "'\n";
            return std::nullopt;
        }
        balance += v;
    }
    return balance;
}

// ─────────────────────────────────────────────────────────────────────────────
// getUTXOsForAddress
// ─────────────────────────────────────────────────────────────────────────────

std::vector<UTXO> UTXOSet::getUTXOsForAddress(
    const std::string &address) const noexcept
{
    std::vector<UTXO> result;
    if (address.empty()) return result;

    std::shared_lock<std::shared_mutex> lock(rwMutex_);

    const auto idxIt = addressIndex_.find(address);
    if (idxIt == addressIndex_.end()) return result;

    result.reserve(idxIt->second.size());
    for (const auto &key : idxIt->second) {
        const auto utxoIt = utxos_.find(key);
        if (utxoIt != utxos_.end())
            result.push_back(utxoIt->second);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// getUTXO
// ─────────────────────────────────────────────────────────────────────────────

std::optional<UTXO> UTXOSet::getUTXO(const std::string &txHash,
                                       int                outputIndex)
    const noexcept
{
    if (txHash.empty() || outputIndex < 0) return std::nullopt;

    std::shared_lock<std::shared_mutex> lock(rwMutex_);

    const auto it = utxos_.find(makeKey(txHash, outputIndex));
    if (it == utxos_.end()) return std::nullopt;
    return it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// isUnspent
// ─────────────────────────────────────────────────────────────────────────────

bool UTXOSet::isUnspent(const std::string &txHash,
                         int                outputIndex) const noexcept
{
    if (txHash.empty() || outputIndex < 0) return false;

    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    return utxos_.count(makeKey(txHash, outputIndex)) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// size
// ─────────────────────────────────────────────────────────────────────────────

size_t UTXOSet::size() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(rwMutex_);
    return utxos_.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// clear
// ─────────────────────────────────────────────────────────────────────────────

void UTXOSet::clear() noexcept
{
    std::unique_lock<std::shared_mutex> lock(rwMutex_);
    utxos_.clear();
    addressIndex_.clear();
    persist();   // called while write lock is held
}

// ─────────────────────────────────────────────────────────────────────────────
// loadSnapshot
//
// Every entry's key is re-derived from its txHash and outputIndex fields
// and compared against the map key under which it is stored. This resolves
// the prior defect where `key` was used before being defined, and ensures
// that a snapshot with internally inconsistent keys is rejected before any
// live state is modified.
// ─────────────────────────────────────────────────────────────────────────────

bool UTXOSet::loadSnapshot(
    const std::unordered_map<std::string, UTXO> &snapshot) noexcept
{
    // Validate all entries against freshly derived keys before touching
    // live state. This is a read-only pass so no lock is needed yet.
    for (const auto &[storedKey, utxo] : snapshot) {
        if (utxo.txHash.empty()) {
            std::cerr << "[UTXOSet] loadSnapshot: entry has empty txHash — "
                         "snapshot rejected\n";
            return false;
        }
        if (utxo.outputIndex < 0) {
            std::cerr << "[UTXOSet] loadSnapshot: entry has negative "
                         "outputIndex — snapshot rejected\n";
            return false;
        }
        if (utxo.address.empty()) {
            std::cerr << "[UTXOSet] loadSnapshot: entry has empty address — "
                         "snapshot rejected\n";
            return false;
        }

        // Re-derive the expected key from the UTXO's own fields and compare
        // against the map key under which it is stored. Both variables are
        // defined locally here, eliminating the prior undefined-variable defect.
        const std::string derivedKey = makeKey(utxo.txHash, utxo.outputIndex);
        if (storedKey != derivedKey) {
            std::cerr << "[UTXOSet] loadSnapshot: key mismatch — "
                         "stored='" << storedKey
                      << "' derived='" << derivedKey
                      << "' — snapshot rejected\n";
            return false;
        }
    }

    // All entries passed validation; now acquire the write lock and replace
    // live state atomically.
    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    utxos_        = snapshot;
    addressIndex_.clear();

    for (const auto &[key, utxo] : utxos_)
        addressIndex_[utxo.address].push_back(key);

    persist();   // called while write lock is held
    return true;
}
