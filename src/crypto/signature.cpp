#include "crypto/signature.h"
#include "crypto/secp256k1_wrapper.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

// Decodes a 64-character lowercase or uppercase hex string into a 32-byte key.
// Validates every character and throws std::runtime_error on any violation.
static std::array<uint8_t, 32> decodeHexKey(const std::string &hex,
                                              const char        *context)
{
    if (hex.size() != 64)
        throw std::runtime_error(
            std::string(context) + ": expected exactly 64 hex characters, got "
            + std::to_string(hex.size()));

    std::array<uint8_t, 32> key{};
    for (size_t i = 0; i < 32; ++i) {
        const char hi = static_cast<char>(
            std::tolower(static_cast<unsigned char>(hex[i * 2])));
        const char lo = static_cast<char>(
            std::tolower(static_cast<unsigned char>(hex[i * 2 + 1])));

        if (!std::isxdigit(static_cast<unsigned char>(hi)) ||
            !std::isxdigit(static_cast<unsigned char>(lo)))
            throw std::runtime_error(
                std::string(context) + ": invalid hex character at offset "
                + std::to_string(i * 2));

        const uint8_t hiByte = std::isdigit(static_cast<unsigned char>(hi))
                               ? static_cast<uint8_t>(hi - '0')
                               : static_cast<uint8_t>(hi - 'a' + 10);
        const uint8_t loByte = std::isdigit(static_cast<unsigned char>(lo))
                               ? static_cast<uint8_t>(lo - '0')
                               : static_cast<uint8_t>(lo - 'a' + 10);

        key[i] = static_cast<uint8_t>((hiByte << 4) | loByte);
    }
    return key;
}

// Reads a private key from a UTF-8 text file.
// The first non-blank, non-comment line must contain exactly 64 hex chars.
// Lines beginning with '#' or '-' are treated as comments and skipped.
static std::array<uint8_t, 32> loadPrivkeyHex(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error(
            "[signature] loadPrivkeyHex: cannot open '" + path + "'");

    std::string line;
    while (std::getline(f, line)) {
        // Strip all whitespace (handles \r\n, trailing spaces, etc.)
        line.erase(
            std::remove_if(line.begin(), line.end(),
                           [](unsigned char c) { return std::isspace(c); }),
            line.end());

        if (line.empty() || line[0] == '#' || line[0] == '-') continue;

        // First usable line — decode and return immediately
        return decodeHexKey(line, "[signature] loadPrivkeyHex");
    }

    throw std::runtime_error(
        "[signature] loadPrivkeyHex: no valid key line found in '" + path + "'");
}

// ─────────────────────────────────────────────────────────────────────────────
// signHash — loads key from file, signs, zeros key, returns result
// ─────────────────────────────────────────────────────────────────────────────

std::tuple<std::array<uint8_t, 32>, std::array<uint8_t, 32>, uint8_t>
signHash(const std::array<uint8_t, 32> &digest,
         const std::string             &privKeyPath)
{
    std::array<uint8_t, 32> privkey = loadPrivkeyHex(privKeyPath);

    // Sign before zeroing so the key is always cleared even if signing throws
    std::optional<crypto::Secp256k1Signature> sigOpt;
    try {
        sigOpt = crypto::signRecoverable(digest.data(), privkey);
    } catch (...) {
        std::memset(privkey.data(), 0, privkey.size());
        throw;
    }
    std::memset(privkey.data(), 0, privkey.size());

    if (!sigOpt)
        throw std::runtime_error(
            "[signature] signHash: signing failed");

    if (sigOpt->recid < 0 || sigOpt->recid > 3)
        throw std::runtime_error(
            "[signature] signHash: invalid recid "
            + std::to_string(sigOpt->recid));

    return { sigOpt->r, sigOpt->s, static_cast<uint8_t>(sigOpt->recid) };
}

// ─────────────────────────────────────────────────────────────────────────────
// signHashWithKey — accepts key directly, zeros local copy, returns result
// ─────────────────────────────────────────────────────────────────────────────

std::tuple<std::array<uint8_t, 32>, std::array<uint8_t, 32>, uint8_t>
signHashWithKey(const std::array<uint8_t, 32> &digest,
                std::array<uint8_t, 32>         privkey)   // value — caller's copy unaffected
{
    // Validate the digest is non-zero (a zero digest likely indicates a
    // caller bug and should never be signed)
    bool digestIsZero = true;
    for (auto b : digest) if (b) { digestIsZero = false; break; }
    if (digestIsZero) {
        std::memset(privkey.data(), 0, privkey.size());
        throw std::runtime_error(
            "[signature] signHashWithKey: refusing to sign a zero digest");
    }

    std::optional<crypto::Secp256k1Signature> sigOpt;
    try {
        sigOpt = crypto::signRecoverable(digest.data(), privkey);
    } catch (...) {
        std::memset(privkey.data(), 0, privkey.size());
        throw;
    }
    std::memset(privkey.data(), 0, privkey.size());

    if (!sigOpt)
        throw std::runtime_error(
            "[signature] signHashWithKey: signing failed");

    if (sigOpt->recid < 0 || sigOpt->recid > 3)
        throw std::runtime_error(
            "[signature] signHashWithKey: invalid recid "
            + std::to_string(sigOpt->recid));

    return { sigOpt->r, sigOpt->s, static_cast<uint8_t>(sigOpt->recid) };
}
