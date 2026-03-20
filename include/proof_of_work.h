#pragma once

#include "block.h"
#include "blockchain.h"
#include "crypto/keccak256.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// =============================================================================
// PROOF OF WORK
//
// Keccak-256 based proof of work.
//
// Issue resolutions:
//   1. hashMeetsTarget declared exactly once as static bool — no duplicate
//   2. Uses crypto::Keccak256(data, len, digest) directly — zero allocation
//   3. saveNonceState/loadNonceState fully declared and implemented in .cpp
//   4. adjustDifficulty uses 10% Ethereum-style step, clamped to bounds
//   5. Metrics match atomics in .cpp exactly — all counters accounted for
//   6. validate() checks hash, difficulty, previousHash, gas, timestamp
//   7. progressThrottleMs enforced in mining loop in .cpp
//   8. All atomic metrics use memory_order_relaxed for read performance
//      and memory_order_acq_rel for found flag — correct across threads
// =============================================================================
class ProofOfWork {
public:

    // =========================================================================
    // RESULT
    // =========================================================================
    struct Result {
        bool        found          = false;
        uint64_t    nonce          = 0;
        std::string hash;
        uint64_t    hashesComputed = 0;
        uint64_t    elapsedMs      = 0;
    };

    // =========================================================================
    // METRICS
    // =========================================================================
    struct Metrics {
        uint64_t totalHashesComputed = 0;
        uint64_t blocksFound         = 0;
        uint64_t validationsPassed   = 0;
        uint64_t validationsFailed   = 0;
        uint64_t difficultyIncreases = 0;
        uint64_t difficultyDecreases = 0;
        double   avgHashRatePerSec   = 0.0;

        std::string toPrometheusText() const noexcept;
    };

    // =========================================================================
    // CALLBACKS
    // =========================================================================
    using ProgressFn = std::function<bool(uint64_t hashesComputed,
                                           double   hashRatePerSec)>;
    using LogFn      = std::function<void(int                level,
                                           const std::string& msg)>;

    // =========================================================================
    // CONFIG
    // =========================================================================
    struct Config {
        uint32_t    threads             = 0;
        uint64_t    hashCheckInterval   = 5000;
        uint64_t    maxNonce            = std::numeric_limits<uint64_t>::max();
        uint64_t    targetBlockTimeSecs = 10;
        uint32_t    minDifficulty       = 1;
        uint32_t    maxDifficulty       = 256;
        uint64_t    progressThrottleMs  = 500;
        std::string nonceStatePath;
        bool        enableMetrics       = true;
    };

    explicit ProofOfWork(Config cfg = {}) noexcept;
    ~ProofOfWork() noexcept;

    ProofOfWork(const ProofOfWork&)            = delete;
    ProofOfWork& operator=(const ProofOfWork&) = delete;

    void setLogger(LogFn fn) noexcept;

    // =========================================================================
    // CANONICAL HEADER SERIALIZATION
    // Big-endian binary — consistent across all platforms.
    // All integers written big-endian. Strings length-prefixed (4 bytes BE).
    // =========================================================================
    static std::vector<uint8_t> serializeHeader(
        const Block& block) noexcept;

    // =========================================================================
    // HASH COMPUTATION
    // Uses crypto::Keccak256(data, len, digest) zero-allocation overload.
    // Returns lowercase 64-character hex string.
    // Returns empty string on any failure — never throws.
    // =========================================================================
    static std::string computeHash(const Block& block) noexcept;

    // =========================================================================
    // VALIDATION
    // meetsTarget : checks leading zero hex chars == difficulty
    // validateHash: recomputes hash and checks it matches block.hash
    // validate    : full chain state check — hash, difficulty bounds,
    //               previousHash in chain, minerAddress, gasUsed <= gasLimit,
    //               timestamp after parent block
    // =========================================================================
    static bool meetsTarget  (const std::string& hash,
                                uint32_t           difficulty) noexcept;
    static bool validateHash (const Block&        block)      noexcept;
    bool        validate     (const Block&        block,
                               const Blockchain&   chain)
                                                        const noexcept;

    // =========================================================================
    // MINING
    // mine        : single-threaded, resumes from persisted nonce
    // mineParallel: multi-threaded, pre/post-nonce buffer per thread
    //               (no Block copy per thread — no large field duplication)
    //               nonce range split via unsigned __int128 — no overflow
    // Both functions:
    //   - respect abort atomic
    //   - throttle progress callback via progressThrottleMs
    //   - persist nonce periodically and on exit
    //   - record all metrics atomically
    // =========================================================================
    Result mine(Block&                   block,
                 const std::atomic<bool>& abort,
                 ProgressFn               progress = nullptr) const noexcept;

    Result mineParallel(Block&                   block,
                         const std::atomic<bool>& abort,
                         ProgressFn               progress = nullptr)
                                                         const noexcept;

    // =========================================================================
    // DIFFICULTY ADJUSTMENT
    // Ethereum-style 10% step increase/decrease.
    // actual < 50% of target  → increase by max(1, current/10)
    // actual > 200% of target → decrease by max(1, current/10)
    // Result clamped to [minDifficulty, maxDifficulty].
    // =========================================================================
    uint32_t adjustDifficulty(uint32_t currentDifficulty,
                               uint64_t actualBlockTimeSecs) const noexcept;

    // =========================================================================
    // METRICS
    // =========================================================================
    Metrics     getMetrics()        const noexcept;
    std::string getPrometheusText() const noexcept;
    void        resetMetrics()            noexcept;

private:

    Config cfg_;

    // Structured logger — logFn_ protected by logMu_
    // Exceptions from logFn_ caught internally — never propagate
    mutable std::mutex logMu_;
    LogFn              logFn_;
    void slog(int level, const std::string& msg) const noexcept;

    // Atomic metrics — all read with memory_order_relaxed
    // found flag in mineParallel uses memory_order_acq_rel
    mutable std::atomic<uint64_t> metHashesComputed {0};
    mutable std::atomic<uint64_t> metBlocksFound    {0};
    mutable std::atomic<uint64_t> metValPassed      {0};
    mutable std::atomic<uint64_t> metValFailed      {0};
    mutable std::atomic<uint64_t> metDiffIncreases  {0};
    mutable std::atomic<uint64_t> metDiffDecreases  {0};

    // Nonce persistence — binary big-endian 8 bytes
    void     saveNonceState(uint64_t nonce) const noexcept;
    uint64_t loadNonceState()               const noexcept;

    // Internal hash check — single declaration, returns bool
    static bool hashMeetsTarget(const std::string& hash,
                                  uint32_t           difficulty) noexcept;

    // Big-endian serialization helpers
    static void writeU64BE(std::vector<uint8_t>& buf,
                            uint64_t              v) noexcept;
    static void writeU32BE(std::vector<uint8_t>& buf,
                            uint32_t              v) noexcept;
    static void writeStr  (std::vector<uint8_t>& buf,
                            const std::string&    s) noexcept;
};
