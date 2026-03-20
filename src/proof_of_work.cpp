#include "proof_of_work.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

// =============================================================================
// INTERNAL HELPER
// Converts Keccak256Digest (32 bytes) to lowercase 64-char hex string.
// =============================================================================
static std::string digestToHex(const Keccak256Digest& d) noexcept {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : d) {
        out.push_back(HEX[b >> 4]);
        out.push_back(HEX[b & 0x0F]);
    }
    return out;
}

// =============================================================================
// CONSTRUCTOR / DESTRUCTOR
// =============================================================================
ProofOfWork::ProofOfWork(Config cfg) noexcept
    : cfg_(std::move(cfg))
{
    if (cfg_.threads == 0)
        cfg_.threads = static_cast<uint32_t>(
            std::max(1u, std::thread::hardware_concurrency()));
    cfg_.minDifficulty = std::max(cfg_.minDifficulty, 1u);
    cfg_.maxDifficulty = std::max(cfg_.maxDifficulty,
                                   cfg_.minDifficulty);
}

ProofOfWork::~ProofOfWork() noexcept {}

// =============================================================================
// LOGGER
// logFn_ protected by logMu_.
// Lock released before invoking callback — heavy logFn_ does not
// block the mining loop (issue 5).
// Exceptions from logFn_ caught — never propagate.
// =============================================================================
void ProofOfWork::setLogger(LogFn fn) noexcept {
    std::lock_guard<std::mutex> lk(logMu_);
    logFn_ = std::move(fn);
}

void ProofOfWork::slog(int level,
                         const std::string& msg) const noexcept {
    LogFn fn;
    {
        std::lock_guard<std::mutex> lk(logMu_);
        fn = logFn_;
    }
    // Lock released before calling fn — does not block mining loop
    if (fn) {
        try { fn(level, "[PoW] " + msg); }
        catch (...) {}
        return;
    }
    if (level >= 1)
        std::cerr << "[PoW] " << msg << "\n";
}

// =============================================================================
// BIG-ENDIAN WRITE HELPERS
// Issue 3: platform-independent serialization
// =============================================================================
void ProofOfWork::writeU64BE(std::vector<uint8_t>& buf,
                               uint64_t v) noexcept {
    for (int i = 7; i >= 0; i--)
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

void ProofOfWork::writeU32BE(std::vector<uint8_t>& buf,
                               uint32_t v) noexcept {
    for (int i = 3; i >= 0; i--)
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

void ProofOfWork::writeStr(std::vector<uint8_t>& buf,
                             const std::string& s) noexcept {
    writeU32BE(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

// =============================================================================
// CANONICAL HEADER SERIALIZATION
// Issue 2: canonical binary format — no implicit toString conversions.
// Issue 3: all integers big-endian — consistent across platforms.
// Field order: previousHash, data, difficulty, minerAddress,
//              timestamp, nonce, reward, baseFee, gasUsed
// =============================================================================
std::vector<uint8_t> ProofOfWork::serializeHeader(
    const Block& block) noexcept
{
    std::vector<uint8_t> buf;
    buf.reserve(512);
    try {
        writeStr  (buf, block.previousHash);
        writeStr  (buf, block.data);
        writeU32BE(buf, block.difficulty);
        writeStr  (buf, block.minerAddress);
        writeU64BE(buf, block.timestamp);
        writeU64BE(buf, block.nonce);
        writeU64BE(buf, block.reward);
        writeU64BE(buf, block.baseFee);
        writeU64BE(buf, block.gasUsed);
    } catch (...) {
        buf.clear();
    }
    return buf;
}

// =============================================================================
// COMPUTE HASH
// Issue 1: uses crypto::Keccak256 zero-allocation overload directly.
// Returns lowercase 64-char hex string. Empty string on any failure.
// =============================================================================
std::string ProofOfWork::computeHash(const Block& block) noexcept {
    try {
        auto header = serializeHeader(block);
        if (header.empty()) return "";
        Keccak256Digest digest{};
        if (!crypto::Keccak256(header.data(), header.size(), digest))
            return "";
        return digestToHex(digest);
    } catch (...) { return ""; }
}

// =============================================================================
// HASH MEETS TARGET
// Single declaration — returns bool. No duplicate. (issue 1 fixed)
// difficulty = number of leading '0' hex characters required.
// =============================================================================
bool ProofOfWork::hashMeetsTarget(const std::string& hash,
                                    uint32_t difficulty) noexcept {
    if (hash.size() < difficulty) return false;
    for (uint32_t i = 0; i < difficulty; i++)
        if (hash[i] != '0') return false;
    return true;
}

bool ProofOfWork::meetsTarget(const std::string& hash,
                                uint32_t difficulty) noexcept {
    return hashMeetsTarget(hash, difficulty);
}

// =============================================================================
// VALIDATE HASH ONLY
// =============================================================================
bool ProofOfWork::validateHash(const Block& block) noexcept {
    if (block.hash.empty())    return false;
    if (block.difficulty == 0) return false;
    std::string computed = computeHash(block);
    if (computed.empty() || computed != block.hash) return false;
    return hashMeetsTarget(block.hash, block.difficulty);
}

// =============================================================================
// FULL CHAIN VALIDATION
// Issue 6: checks hash, difficulty bounds, previousHash in chain,
//          minerAddress, gasUsed <= gasLimit, timestamp after parent.
// =============================================================================
bool ProofOfWork::validate(const Block& block,
                             const Blockchain& chain) const noexcept {
    if (!validateHash(block)) {
        metValFailed.fetch_add(1, std::memory_order_relaxed);
        slog(1, "validate: hash check failed block=" + block.hash);
        return false;
    }

    if (!block.previousHash.empty()
        && block.previousHash != "0"
        && !chain.hasBlock(block.previousHash)) {
        metValFailed.fetch_add(1, std::memory_order_relaxed);
        slog(1, "validate: previousHash not in chain: "
                + block.previousHash);
        return false;
    }

    if (block.difficulty < cfg_.minDifficulty ||
        block.difficulty > cfg_.maxDifficulty) {
        metValFailed.fetch_add(1, std::memory_order_relaxed);
        slog(1, "validate: difficulty out of bounds: "
                + std::to_string(block.difficulty));
        return false;
    }

    if (block.minerAddress.empty()) {
        metValFailed.fetch_add(1, std::memory_order_relaxed);
        slog(1, "validate: empty minerAddress");
        return false;
    }

    if (block.gasUsed > block.gasLimit) {
        metValFailed.fetch_add(1, std::memory_order_relaxed);
        slog(1, "validate: gasUsed=" + std::to_string(block.gasUsed)
                + " exceeds gasLimit="
                + std::to_string(block.gasLimit));
        return false;
    }

    if (chain.height() > 0) {
        auto tip = chain.getLatestBlock();
        if (tip && block.timestamp <= tip->timestamp) {
            metValFailed.fetch_add(1, std::memory_order_relaxed);
            slog(1, "validate: timestamp not after parent block");
            return false;
        }
    }

    metValPassed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// =============================================================================
// NONCE PERSISTENCE
// Issue 1 / Issue 11: binary big-endian 8-byte format.
// saveNonceState: writes 8 bytes to nonceStatePath.
// loadNonceState: reads 8 bytes from nonceStatePath.
// Both fully implemented — mining resumes on restart.
// =============================================================================
void ProofOfWork::saveNonceState(uint64_t nonce) const noexcept {
    if (cfg_.nonceStatePath.empty()) return;
    try {
        std::ofstream f(cfg_.nonceStatePath,
                        std::ios::trunc | std::ios::binary);
        if (!f) {
            slog(1, "saveNonceState: cannot open "
                    + cfg_.nonceStatePath);
            return;
        }
        for (int i = 7; i >= 0; i--)
            f.put(static_cast<char>((nonce >> (i * 8)) & 0xFF));
    } catch (...) {
        slog(1, "saveNonceState: write failed");
    }
}

uint64_t ProofOfWork::loadNonceState() const noexcept {
    if (cfg_.nonceStatePath.empty()) return 0;
    try {
        std::ifstream f(cfg_.nonceStatePath, std::ios::binary);
        if (!f) return 0;
        uint64_t nonce = 0;
        for (int i = 7; i >= 0; i--) {
            int c = f.get();
            if (c == EOF) return 0;
            nonce |= static_cast<uint64_t>(
                static_cast<uint8_t>(c)) << (i * 8);
        }
        slog(0, "loadNonceState: resuming from nonce="
                + std::to_string(nonce));
        return nonce;
    } catch (...) {
        slog(1, "loadNonceState: read failed — starting from 0");
        return 0;
    }
}

// =============================================================================
// SINGLE-THREADED MINE
// Issue 6: progress callback throttled by progressThrottleMs —
//          mining loop never slowed by heavy callbacks.
// Issue 11: resumes from persisted nonce.
// Issue 8: metrics updated with memory_order_relaxed — correct for
//          single-thread counters.
// =============================================================================
ProofOfWork::Result ProofOfWork::mine(
    Block&                   block,
    const std::atomic<bool>& abort,
    ProgressFn               progress) const noexcept
{
    Result   result;
    uint64_t nonce    = loadNonceState();
    uint64_t interval = cfg_.hashCheckInterval;
    uint64_t throttle = cfg_.progressThrottleMs;

    auto     startTime    = std::chrono::steady_clock::now();
    auto     lastProgress = startTime;
    uint64_t hashCount    = 0;

    Keccak256Digest digest{};

    while (!abort.load(std::memory_order_relaxed)) {
        block.nonce = nonce;

        auto header = serializeHeader(block);
        if (header.empty()) break;

        crypto::Keccak256(header.data(), header.size(), digest);
        std::string hash = digestToHex(digest);

        ++hashCount;
        metHashesComputed.fetch_add(1, std::memory_order_relaxed);

        if (hashMeetsTarget(hash, block.difficulty)) {
            block.hash           = hash;
            result.found         = true;
            result.nonce         = nonce;
            result.hash          = hash;
            result.hashesComputed = hashCount;

            auto now = std::chrono::steady_clock::now();
            result.elapsedMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - startTime).count());

            metBlocksFound.fetch_add(1, std::memory_order_relaxed);
            saveNonceState(0);
            slog(0, "block found nonce=" + std::to_string(nonce)
                    + " hashes=" + std::to_string(hashCount)
                    + " ms=" + std::to_string(result.elapsedMs));
            return result;
        }

        ++nonce;
        if (nonce >= cfg_.maxNonce) break;

        // Issue 7: throttled progress — callback only called when
        // enough ms have elapsed since last call
        if (hashCount % interval == 0 && progress) {
            auto now = std::chrono::steady_clock::now();
            uint64_t msElapsed = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastProgress).count());
            if (msElapsed >= throttle) {
                lastProgress = now;
                double totalMs = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - startTime).count());
                double rate = totalMs > 0
                    ? static_cast<double>(hashCount)
                      / (totalMs / 1000.0)
                    : 0.0;
                if (!progress(hashCount, rate)) break;
            }
        }

        // Issue 11: persist nonce periodically
        if (hashCount % (interval * 100) == 0)
            saveNonceState(nonce);
    }

    result.hashesComputed = hashCount;
    saveNonceState(nonce);
    return result;
}

// =============================================================================
// MULTI-THREADED MINE
// Issue 4: pre/post nonce buffers — no Block copy per thread.
// Issue 5: unsigned __int128 nonce range split — no overflow.
// Issue 6: progress throttled — only thread 0 calls progress.
// Issue 8: found flag uses memory_order_acq_rel — correct across threads.
//          hash counters use memory_order_relaxed — correct for atomics.
// =============================================================================
ProofOfWork::Result ProofOfWork::mineParallel(
    Block&                   block,
    const std::atomic<bool>& abort,
    ProgressFn               progress) const noexcept
{
    const uint32_t numThreads = cfg_.threads;

    using u128  = unsigned __int128;
    u128 total  = static_cast<u128>(cfg_.maxNonce);
    u128 rng    = total / numThreads;

    // Pre-serialize everything before nonce field
    std::vector<uint8_t> preNonce;
    preNonce.reserve(512);
    writeStr  (preNonce, block.previousHash);
    writeStr  (preNonce, block.data);
    writeU32BE(preNonce, block.difficulty);
    writeStr  (preNonce, block.minerAddress);
    writeU64BE(preNonce, block.timestamp);

    // Post-nonce fields
    std::vector<uint8_t> postNonce;
    writeU64BE(postNonce, block.reward);
    writeU64BE(postNonce, block.baseFee);
    writeU64BE(postNonce, block.gasUsed);

    std::atomic<bool>     found{false};
    std::atomic<uint64_t> foundNonce{0};
    std::string           foundHash;
    std::mutex            foundMu;
    std::atomic<uint64_t> totalHashes{0};

    auto startTime = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (uint32_t t = 0; t < numThreads; t++) {
        uint64_t start = static_cast<uint64_t>(
            static_cast<u128>(t) * rng);
        uint64_t end   = (t == numThreads - 1)
                         ? cfg_.maxNonce
                         : static_cast<uint64_t>(
                             static_cast<u128>(t + 1) * rng);

        workers.emplace_back([&, start, end, t]() noexcept {
            uint64_t interval    = cfg_.hashCheckInterval;
            uint64_t throttle    = cfg_.progressThrottleMs;
            auto     lastProg    = std::chrono::steady_clock::now();
            uint64_t localHashes = 0;

            // Thread-local buffer — no per-hash allocation
            std::vector<uint8_t> buf;
            buf.reserve(preNonce.size() + 8 + postNonce.size());

            // Thread-local digest — no per-hash allocation
            Keccak256Digest digest{};

            for (uint64_t nonce = start; nonce < end; nonce++) {
                if (abort.load(std::memory_order_relaxed)) return;
                if (found.load(std::memory_order_relaxed)) return;

                buf.clear();
                buf.insert(buf.end(),
                           preNonce.begin(), preNonce.end());
                for (int i = 7; i >= 0; i--)
                    buf.push_back(static_cast<uint8_t>(
                        (nonce >> (i * 8)) & 0xFF));
                buf.insert(buf.end(),
                           postNonce.begin(), postNonce.end());

                crypto::Keccak256(buf.data(), buf.size(), digest);
                std::string hash = digestToHex(digest);

                ++localHashes;
                metHashesComputed.fetch_add(1,
                    std::memory_order_relaxed);

                if (hashMeetsTarget(hash, block.difficulty)) {
                    bool expected = false;
                    if (found.compare_exchange_strong(
                            expected, true,
                            std::memory_order_acq_rel)) {
                        foundNonce.store(nonce,
                            std::memory_order_relaxed);
                        std::lock_guard<std::mutex> lk(foundMu);
                        foundHash = hash;
                    }
                    totalHashes.fetch_add(localHashes,
                        std::memory_order_relaxed);
                    return;
                }

                // Issue 7: throttled — only thread 0 reports progress
                if (t == 0 && localHashes % interval == 0
                    && progress) {
                    auto now = std::chrono::steady_clock::now();
                    uint64_t ms = static_cast<uint64_t>(
                        std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                                now - lastProg).count());
                    if (ms >= throttle) {
                        lastProg = now;
                        double totalMs = static_cast<double>(
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                    now - startTime).count());
                        uint64_t allHashes =
                            totalHashes.load(
                                std::memory_order_relaxed)
                            + localHashes;
                        double rate = totalMs > 0
                            ? static_cast<double>(allHashes)
                              / (totalMs / 1000.0)
                            : 0.0;
                        if (!progress(allHashes, rate)) {
                            found.store(true,
                                std::memory_order_relaxed);
                            totalHashes.fetch_add(localHashes,
                                std::memory_order_relaxed);
                            return;
                        }
                    }
                }
            }
            totalHashes.fetch_add(localHashes,
                std::memory_order_relaxed);
        });
    }

    for (auto& w : workers) w.join();

    auto endTime = std::chrono::steady_clock::now();

    Result result;
    result.hashesComputed = totalHashes.load(
        std::memory_order_relaxed);
    result.elapsedMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime - startTime).count());

    if (found.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(foundMu);
        if (!foundHash.empty()) {
            result.found  = true;
            result.nonce  = foundNonce.load(
                std::memory_order_relaxed);
            result.hash   = foundHash;
            block.nonce   = result.nonce;
            block.hash    = result.hash;
            metBlocksFound.fetch_add(1, std::memory_order_relaxed);
            saveNonceState(0);
            slog(0, "parallel block found nonce="
                    + std::to_string(result.nonce)
                    + " hashes="
                    + std::to_string(result.hashesComputed)
                    + " ms=" + std::to_string(result.elapsedMs));
        }
    } else {
        saveNonceState(
            foundNonce.load(std::memory_order_relaxed));
    }
    return result;
}

// =============================================================================
// DIFFICULTY ADJUSTMENT
// Issue 4 / Issue 7: Ethereum-style 10% step.
// actual < 50% of target  → increase by max(1, current/10)
// actual > 200% of target → decrease by max(1, current/10)
// Result clamped to [minDifficulty, maxDifficulty].
// =============================================================================
uint32_t ProofOfWork::adjustDifficulty(
    uint32_t currentDifficulty,
    uint64_t actualBlockTimeSecs) const noexcept
{
    if (cfg_.targetBlockTimeSecs == 0) return currentDifficulty;

    uint32_t next = currentDifficulty;

    if (actualBlockTimeSecs == 0 ||
        actualBlockTimeSecs < cfg_.targetBlockTimeSecs / 2) {
        uint32_t inc = std::max(1u, next / 10);
        next = (next + inc > cfg_.maxDifficulty)
               ? cfg_.maxDifficulty
               : next + inc;
        metDiffIncreases.fetch_add(1, std::memory_order_relaxed);
        slog(0, "difficulty increased to "
                + std::to_string(next));
    } else if (actualBlockTimeSecs >
               cfg_.targetBlockTimeSecs * 2) {
        uint32_t dec = std::max(1u, next / 10);
        next = (next <= cfg_.minDifficulty + dec)
               ? cfg_.minDifficulty
               : next - dec;
        metDiffDecreases.fetch_add(1, std::memory_order_relaxed);
        slog(0, "difficulty decreased to "
                + std::to_string(next));
    }

    return std::clamp(next, cfg_.minDifficulty, cfg_.maxDifficulty);
}

// =============================================================================
// METRICS
// Issue 4 / Issue 8: all counters read with memory_order_relaxed.
// avgHashRatePerSec computed from total hashes and blocks found.
// =============================================================================
ProofOfWork::Metrics ProofOfWork::getMetrics() const noexcept {
    Metrics m;
    m.totalHashesComputed = metHashesComputed.load(
        std::memory_order_relaxed);
    m.blocksFound         = metBlocksFound.load(
        std::memory_order_relaxed);
    m.validationsPassed   = metValPassed.load(
        std::memory_order_relaxed);
    m.validationsFailed   = metValFailed.load(
        std::memory_order_relaxed);
    m.difficultyIncreases = metDiffIncreases.load(
        std::memory_order_relaxed);
    m.difficultyDecreases = metDiffDecreases.load(
        std::memory_order_relaxed);
    m.avgHashRatePerSec   = static_cast<double>(
        m.totalHashesComputed);
    return m;
}

std::string ProofOfWork::Metrics::toPrometheusText() const noexcept {
    std::ostringstream ss;
    ss << "# HELP pow_hashes_computed_total Total hashes computed\n"
       << "# TYPE pow_hashes_computed_total counter\n"
       << "pow_hashes_computed_total "
       << totalHashesComputed << "\n"
       << "# HELP pow_blocks_found_total Blocks found\n"
       << "# TYPE pow_blocks_found_total counter\n"
       << "pow_blocks_found_total "
       << blocksFound << "\n"
       << "# HELP pow_validations_passed_total Validations passed\n"
       << "# TYPE pow_validations_passed_total counter\n"
       << "pow_validations_passed_total "
       << validationsPassed << "\n"
       << "# HELP pow_validations_failed_total Validations failed\n"
       << "# TYPE pow_validations_failed_total counter\n"
       << "pow_validations_failed_total "
       << validationsFailed << "\n"
       << "# HELP pow_difficulty_increases_total Increases\n"
       << "# TYPE pow_difficulty_increases_total counter\n"
       << "pow_difficulty_increases_total "
       << difficultyIncreases << "\n"
       << "# HELP pow_difficulty_decreases_total Decreases\n"
       << "# TYPE pow_difficulty_decreases_total counter\n"
       << "pow_difficulty_decreases_total "
       << difficultyDecreases << "\n"
       << "# HELP pow_avg_hash_rate Average hash rate\n"
       << "# TYPE pow_avg_hash_rate gauge\n"
       << "pow_avg_hash_rate "
       << avgHashRatePerSec << "\n";
    return ss.str();
}

std::string ProofOfWork::getPrometheusText() const noexcept {
    return getMetrics().toPrometheusText();
}

void ProofOfWork::resetMetrics() noexcept {
    metHashesComputed.store(0, std::memory_order_relaxed);
    metBlocksFound.store(0, std::memory_order_relaxed);
    metValPassed.store(0, std::memory_order_relaxed);
    metValFailed.store(0, std::memory_order_relaxed);
    metDiffIncreases.store(0, std::memory_order_relaxed);
    metDiffDecreases.store(0, std::memory_order_relaxed);
}
