#ifndef KECCAK256_H
#define KECCAK256_H

#include <cstdint>
#include <array>
#include <cstddef>

namespace crypto {

class Keccak256 {
public:
    static constexpr size_t HASH_SIZE = 32;
    Keccak256() noexcept;
    void update(const uint8_t* data, size_t length) noexcept;
    std::array<uint8_t, HASH_SIZE> digest() noexcept;
    void reset() noexcept;

private:
    void keccakf1600(std::array<uint8_t, 200>& state) noexcept;
    std::array<uint8_t, 200> state_;
    size_t rate_;
    size_t pos_;
};

} // namespace crypto

#endif // KECCAK256_H
