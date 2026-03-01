#ifndef KECCAK256_H
#define KECCAK256_H

#include <vector>
#include <cstdint>

namespace crypto {
    std::vector<unsigned char> Keccak256(const std::vector<unsigned char>& data);
}

#endif // KECCAK256_H
