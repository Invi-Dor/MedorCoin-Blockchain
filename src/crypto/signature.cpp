#include "crypto/signature.h"
#include "crypto/secp256k1_wrapper.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>

static std::array<uint8_t, 32> loadPrivkeyHex(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("signHash: cannot open key file: " + path);

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        // strip whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (line.size() != 64)
            throw std::runtime_error("signHash: key file line is not 64 hex chars: " + path);
        for (char c : line) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                throw std::runtime_error("signHash: invalid hex character in key file: " + path);
        }
        std::array<uint8_t, 32> key{};
        for (size_t i = 0; i < 32; ++i) {
            unsigned int byte = 0;
            std::istringstream ss(line.substr(i * 2, 2));
            ss >> std::hex >> byte;
            key[i] = static_cast<uint8_t>(byte);
        }
        return key;
    }
    throw std::runtime_error("signHash: no valid key line found in: " + path);
}

std::tuple<std::array<uint8_t, 32>,
           std::array<uint8_t, 32>,
           uint8_t>
signHash(const std::array<uint8_t, 32> &digest,
         const std::string             &privKeyPath)
{
    auto privkey = loadPrivkeyHex(privKeyPath);
    auto result  = signHashWithKey(digest, privkey);
    privkey.fill(0);
    return result;
}

std::tuple<std::array<uint8_t, 32>,
           std::array<uint8_t, 32>,
           uint8_t>
signHashWithKey(const std::array<uint8_t, 32> &digest,
                std::array<uint8_t, 32>         privkey)
{
    auto sigOpt = crypto::signRecoverable(
        std::span<const uint8_t, 32>(digest.data(),  32),
        std::span<const uint8_t, 32>(privkey.data(), 32));

    privkey.fill(0);

    if (!sigOpt)
        throw std::runtime_error("signHashWithKey: signing failed");

    return { sigOpt->r, sigOpt->s, static_cast<uint8_t>(sigOpt->recid) };
}
