#include "storage.h"

#include <cctype>
#include <cstring>
#include <functional>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// Layout constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t ADDR_HEX_LEN  = 40;   // 20 bytes  → 40 hex chars
static constexpr size_t SLOT_HEX_LEN  = 64;   // 32 bytes  → 64 hex chars
static constexpr size_t BALANCE_BYTES = 32;    // 256-bit   → 32 raw bytes

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

EVMStorage::EVMStorage(const std::string &dbPath)
    : rocksdb(dbPath)
{
    if (!rocksdb.isOpen()) {
        // RocksDBWrapper already logged the error; surface it here too so the
        // owning object knows it was constructed in a degraded state.
        std::cerr << "[EVMStorage] WARNING: underlying RocksDB failed to open at '"
                  << dbPath << "'. All operations will fail safely.\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stripe selection
// ─────────────────────────────────────────────────────────────────────────────

size_t EVMStorage::stripeFor(const std::string &addrHex) const noexcept
{
    return std::hash<std::string>{}(addrHex) % MUTEX_STRIPES;
}

// ─────────────────────────────────────────────────────────────────────────────
// Validation helpers
// ─────────────────────────────────────────────────────────────────────────────

bool EVMStorage::isValidHex(const std::string &s, size_t requiredLen) noexcept
{
    if (s.size() != requiredLen) return false;
    for (unsigned char c : s)
        if (!std::isxdigit(c)) return false;
    return true;
}

bool EVMStorage::isValidAddrHex(const std::string &s) noexcept
{
    return isValidHex(s, ADDR_HEX_LEN);
}

bool EVMStorage::isValidSlotHex(const std::string &s) noexcept
{
    return isValidHex(s, SLOT_HEX_LEN);
}

// ─────────────────────────────────────────────────────────────────────────────
// Contract Code
// ─────────────────────────────────────────────────────────────────────────────

bool EVMStorage::putContractCode(const std::string &addrHex,
                                  const std::vector<uint8_t> &code)
{
    if (!isValidAddrHex(addrHex)) {
        std::cerr << "[EVMStorage] putContractCode: invalid address '"
                  << addrHex << "' (must be " << ADDR_HEX_LEN << " hex chars)\n";
        return false;
    }
    // Empty bytecode is legal (e.g. EOA receiving a deploy that reverts),
    // so we do not reject code.empty() here.

    const std::string value(reinterpret_cast<const char *>(code.data()),
                             code.size());

    std::unique_lock<std::shared_mutex> lock(stripes[stripeFor(addrHex)]);
    bool ok = rocksdb.put(codeKey(addrHex), value, /*sync=*/true);
    if (!ok)
        std::cerr << "[EVMStorage] putContractCode: DB write failed for "
                  << addrHex << "\n";
    return ok;
}

EVMStorage::ReadStatus EVMStorage::getContractCode(const std::string &addrHex,
                                                    std::vector<uint8_t> &codeOut)
{
    codeOut.clear();

    if (!isValidAddrHex(addrHex)) {
        std::cerr << "[EVMStorage] getContractCode: invalid address '"
                  << addrHex << "'\n";
        return ReadStatus::DB_ERROR;
    }

    std::shared_lock<std::shared_mutex> lock(stripes[stripeFor(addrHex)]);

    std::string raw;
    if (!rocksdb.get(codeKey(addrHex), raw))
        return ReadStatus::NOT_FOUND;

    // A stored key that maps to an empty string is a corruption signal —
    // legitimate empty bytecode should not be written via putContractCode
    // for​​​​​​​​​​​​​​​​
