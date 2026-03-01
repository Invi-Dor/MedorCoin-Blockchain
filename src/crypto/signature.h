include <array>
include <tuple>

namespace crypto {

// Returns (r, s, v) where r and s are 32-byte values and v is the recovery id (0..3)
std::tuple<std::array<uint8_t, 32>, std::array<uint8_t, 32>, uint8_t> signHash(
    const std::array<uint8_t, 32>& digest,
    const std::array<uint8_t, 32>& privkey);

} // namespace crypto

endif // SIGNATURE_H
