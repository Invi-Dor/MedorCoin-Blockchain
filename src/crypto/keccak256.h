#ifndef KECCAK256_H
#define KECCAK256_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <array>

namespace crypto {
    std::vector<unsigned char> Keccak256(const std::vector<unsigned char>& data);
}

#endif // KECCAK256_H
