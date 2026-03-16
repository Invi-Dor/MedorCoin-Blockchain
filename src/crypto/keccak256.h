#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace crypto {

/**
 * Keccak256
 *
 * Production Keccak-256 implementation targeting Ethereum semantics.
 *
 * Design guarantees:
 *  - Uses the Keccak domain separator (0x01), which is correct for all
 *    Ethereum hashing operations. This is distinct from NIST SHA-3 (0x06)
 *    and is documented here explicitly to prevent confusion.
 *  - All overloads operate on a single internal implementation that works
 *    directly on raw pointers, eliminating the extra vector copy that the
 *    pointer overload previously introduced.
 *  - A fixed-size output overload writes into a caller-supplied
 *    std::array<uint8_t,32> to allow callers to reuse stack or pre-allocated
 *    heap buffers across repeated calls, eliminating per-call heap allocation
 *    in high-throughput paths.
 *  - Null pointer inputs are rejected and return a zero digest; the caller
 *    is informed via the return value rather than via stderr alone.
 *  - No method throws. All failures are communicated via return values.
 */

// Fixed-size digest type used by the zero-allocation overload
using Keccak256Digest = std::array<uint8_t, 32>;

// Primary overload — writes into a caller-supplied digest.
// Returns true on success, false if data is null and length > 0.
// Use this in hot paths to avoid heap allocation.
bool Keccak256(const uint8_t      *data,
               size_t              length,
               Keccak256Digest    &digestOut) noexcept;

// Convenience overload — returns a heap-allocated vector.
// Suitable for non-critical paths. Returns a 32-byte zero vector on error.
std::vector<uint8_t> Keccak256(const uint8_t *data,
                                size_t         length) noexcept;

// Convenience overload — accepts a vector input.
std::vector<uint8_t> Keccak256(const std::vector<uint8_t> &data) noexcept;

// Convenience overload — writes vector input into a fixed digest.
bool Keccak256(const std::vector<uint8_t> &data,
               Keccak256Digest            &digestOut) noexcept;

} // namespace crypto
