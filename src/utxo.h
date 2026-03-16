#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * UTXOSet
 *
 * Thread-safe, in-memory Unspent Transaction Output set with optional
 * persistence via a caller-supplied commit callback.
 *
 * Design guarantees:
 *  - A shared_mutex allows concurrent readers while writes are exclusive.
 *  - makeKey uses a length-prefixed format to eliminate colon-collision
 *    ambiguity when txHash itself contains a colon character.
 *  - getBalance and getUTXOsForAddress use a secondary address index for
 *    O(1) average lookup rather than a linear scan of all UTXOs.
 *  - Balance overflow returns std::nullopt so the caller decides how to
 *    handle the condition; the program is never terminated unexpectedly.
 *  - persist() is called while the write lock is still held so no other
 *    thread can mutate state between the in-memory change and the callback.
 *  - indexAdd and indexRemove are private and are only ever called from
 *    methods that already hold the write lock, eliminating the possibility
 *    of a data race on the secondary index.
 *  - COINBASE_MATURITY is defined in this header so every translation unit
 *    that includes utxo.h has access to it.
 *  - loadSnapshot validates every entry against a freshly computed key so
 *    the undefined-variable defect from the prior version cannot occur.
 *  - No public method throws. All failures are communicated via bool or
 *    std::optional.
 */

struct TxOutput {
    uint64_t    value   = 0;
    std::string address;
};

struct UTXO {
    std::string txHash;
    int         outputIndex = 0;
    uint64_t    value       = 0;
    std::string address;
    uint64_t    blockHeight = 0;
    bool        isCoinbase  = false;
};

class UTXOSet {
public:

    // Minimum confirmations before a coinbase output may be spent.
    static constexpr uint64_t COINBASE_MATURITY = 100;

    // Persistence callback — invoked while the write lock is held so the
    // snapshot it receives is always consistent with the live state.
    using PersistFn =
        std::function<void(const std::unordered_map<std::string, UTXO> &snapshot)>;

    explicit UTXOSet(PersistFn persistFn = nullptr);

    UTXOSet(const UTXOSet &)            = delete;
    UTXOSet &operator=(const UTXOSet &) = delete;

    // ── Mutation ───────────────────────────────────────────────────────────

    bool addUTXO(const TxOutput    &output,
                 const std::string &txHash,
                 int                outputIndex,
                 uint64_t           blockHeight,
                 bool               isCoinbase = false) noexcept;

    bool spendUTXO(const std::string &txHash,
                   int                outputIndex,
                   uint64_t           currentBlockHeight) noexcept;

    // ── Queries ────────────────────────────────────────────────────────────

    // Returns the summed balance for address, or std::nullopt on overflow.
    // Returns 0 (not nullopt) for an unknown address.
    std::optional<uint64_t> getBalance(const std::string &address)
        const noexcept;

    std::vector<UTXO>   getUTXOsForAddress(const std::string &address)
        const noexcept;

    std::optional<UTXO> getUTXO(const std::string &txHash,
                                  int                outputIndex)
        const noexcept;

    bool   isUnspent(const std::string &txHash,
                     int                outputIndex) const noexcept;

    size_t size()  const noexcept;
    void   clear()       noexcept;

    // Replaces current state atomically from an external snapshot.
    // Every entry is validated and its key is re-derived before any
    // live state is modified, so a malformed snapshot cannot corrupt
    // an otherwise healthy set.
    bool loadSnapshot(
        const std::unordered_map<std::string, UTXO> &snapshot) noexcept;

private:
    std::unordered_map<std::string, UTXO>            utxos_;
    std::unordered_map<std::string,
                       std::vector<std::string>>     addressIndex_;

    mutable std::shared_mutex                        rwMutex_;
    PersistFn                                        persistFn_;

    // Key format: "<hashLen>:<txHash>:<index>"
    // The length prefix prevents collision when txHash contains a colon.
    static std::string makeKey(const std::string &txHash,
                                int                outputIndex) noexcept;

    // Both helpers must be called with the write lock already held.
    void indexAdd   (const std::string &address,
                     const std::string &key) noexcept;
    void indexRemove(const std::string &address,
                     const std::string &key) noexcept;

    // Invoked while the write lock is held so the snapshot is consistent.
    void persist() const noexcept;
};
