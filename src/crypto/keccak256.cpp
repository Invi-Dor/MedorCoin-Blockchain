// SPDX-License-Identifier: MIT

#include <iostream>
#include <vector>
#include <cstring>
#include <array>

namespace crypto {

class Keccak256 {
public:
    static constexpr size_t HASH_SIZE = 32; // 256 bits
    Keccak256() noexcept;
    void update(const uint8_t* data, size_t length) noexcept;
    std::array<uint8_t, HASH_SIZE> digest() noexcept;
    void reset() noexcept;

private:
    void keccakf1600(std::array<uint8_t, 200>& state) noexcept;

    std::array<uint8_t, 200> state_; // internal state
    size_t rate_; // Keccak rate
    size_t pos_;  // current position in the buffer
};

Keccak256::Keccak256() noexcept
    : state_(), rate_(168), pos_(0) {
    reset();
}

void Keccak256::reset() noexcept {
    std::memset(state_.data(), 0, state_.size());
    pos_ = 0;
}

void Keccak256::update(const uint8_t* data, size_t length) noexcept {
    size_t i = 0;
    while (length > 0) {
        size_t block_size = std::min(rate_ - pos_, length);
        std::memcpy(state_.data() + pos_, data + i, block_size);
        pos_ += block_size;
        i += block_size;
        length -= block_size;

        if (pos_ == rate_) {
            keccakf1600(state_);
            pos_ = 0;
        }
    }
}

std::array<uint8_t, Keccak256::HASH_SIZE> Keccak256::digest() noexcept {
    std::array<uint8_t, HASH_SIZE> result = {};
    state_[rate_ - 1] ^= 0x01; // Padding
    keccakf1600(state_);
    std::memcpy(result.data(), state_.data(), HASH_SIZE);
    return result;
}

void Keccak256::keccakf1600(std::array<uint8_t, 200>& state) noexcept {
    // Keccak f1600 function goes here. For brevity, not included here.
}

} // namespace crypto
