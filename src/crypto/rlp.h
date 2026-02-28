#ifndef RLP_H
#define RLP_H

#include <vector>
#include <cstddef>
#include <cstdint>

std::vector<unsigned char> rlp_encode(const std::vector<unsigned char>& input);

#endif // RLP_H
