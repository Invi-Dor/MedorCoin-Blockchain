// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <vector>
#include <cstring> // For memcpy

namespace crypto {
    class Keccak256 {
    public:
        // Constructor
        Keccak256() noexcept;

        // Reset the state of the Keccak256 hash object
        void reset() noexcept;

        // Update the hash with new input data
        void update(const uint8_t* data, size_t len) noexcept;

        // Finalize the hash and return the digest (fixed-size 32 bytes)
        std::array<uint8_t, 32> digest() noexcept;

    private:
        // State for Keccak256 computation
        std::array<uint64_t, 25> state;  // Keccak state (5x5 array of 64-bit words)
        std::vector<uint8_t> buffer;      // Buffer for partial input
        size_t byte_count;                // Total number of input bytes processed

        // Helper function for processing the internal state
        void processBlock() noexcept;
    };
}
