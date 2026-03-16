#include "storage.h"

#include <cctype>
#include <cstring>
#include <iostream>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// Layout constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t ADDR_HEX_LEN  = 40;
static constexpr size_t SLOT_HEX_LEN  = 64;
static constexpr size_t BALANCE_BYTES = 32;

// Code record layout: [4 bytes length, big-endian] [4 bytes CRC32] [N bytes code]
static constexpr size_t CODE_HEADER_BYTES = sizeof(uint32_t) + sizeof(uint32_t);

// ─────────────────────────────────────────────────────────────────────────────
// CRC32 (ISO 3309 polynomial) — detects corruption beyond size checks alone
// ─────────────────────────────────────────────────────────────────────────────

uint32_t EVMStorage::crc32c(const uint8_t *data, size_t length) noexcept
{
    static constexpr uint32_t POLY = 0xEDB88320u;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (POLY & ~((crc & 1) - 1));
    }
    return crc ^ 0xFFFFFFFFu;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

EVMStorage::EVMStorage(const std::string &dbPath)
    : rocksdb(dbPath)
{
    if (!rocksdb.isOpen()) {
        std::cerr << "[EVMStorage] WARNING: RocksDB failed to open at '"
                  << dbPath << "'. All operations will fail safely.\n";
        return;
    }
    if (!initSchemaVersion()) {
        std::cerr << "[EVMStorage] FATAL: schema version mismatch or write "
                     "failure at '" << dbPath << "'. "
                     "Inspect or migrate the database before use.\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Schema versioning — detects incompatible layout changes across deployments
// ─────────────────────────────────────────────────────────────────────────────

bool EVMStorage::initSchemaVersion()
{
    std::string existing;
    bool found = false;
    try { found = rocksdb.get(schemaKey(), existing); }
    catch (const std::exception &e) {
        std::cerr << "[EVMStorage] initSchemaVersion: get threw: "
                  << e.what() << "\n";
        return false;
    }

    if (!found) {
        // First open — write the current version in big-endian
        uint8_t buf[4];
        buf[0] = (SCHEMA_VERSION >> 24) & 0xFF;
        buf[1] = (SCHEMA_VERSION >> 16) & 0xFF;
        buf[2] = (SCHEMA_VERSION >>  8) & 0xFF;
        buf[3] =  SCHEMA_VERSION        & 0xFF;
        const std::string value(reinterpret_cast<const char *>(buf), 4);
        try { return rocksdb.put(schemaKey(), value, /*sync=*/true); }
        catch (const std::exception &e) {
            std::cerr << "[EVMStorage] initSchemaVersion: put threw: "
                      << e.what() << "\n";
            return false;
        }
    }

    // Existing DB — verify version matches
    if (existing.size() != 4) {
        std::cerr << "[EVMStorage] initSchemaVersion: corrupt version record "
                     "(size=" << existing.size() << ")\n";
        return false;
    }
    const uint32_t stored =
        (static_cast<uint32_t>(static_cast<uint8_t>(existing[0])) << 24) |
        (static_cast<uint32_t>(static_cast<uint8_t>(existing[1])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(existing[2])) <<  8) |
         static_cast<uint32_t>(static_cast<uint8_t>(existing[3]));

    if (stored != SCHEMA_VERSION) {
        std::cerr << "[EVMStorage] initSchemaVersion: version mismatch — "
                     "DB has v" << stored << ", code expects v"
                  << SCHEMA_VERSION << "\n";
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-slot mutex lookup
// ─────────────────────────────────────────────────────────────────────────────

std::shared_mutex &EVMStorage::mutexForKey(const std::string &dbKey) const
{
    std::lock_guard<std::mutex> mapLock(slotMapMutex);
    return slotMutexMap[dbKey];
}

// ─────────────────────────────────────────────────────────────────────────────
// Validation
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
// Contract Bytecode
// ─────────────────────────────────────────────────────────────────────────────

bool EVMStorage::putContractCode(const std::string &addrHex,
                                  const std::vector<uint8_t> &code)
{
    if (!isValidAddrHex(addrHex)) {
        std::cerr << "[EVMStorage] putContractCode: invalid address '"
                  << addrHex << "' — must be exactly "
                  << ADDR_HEX_LEN << " valid hex characters\n";
        return false;
    }

    // Record layout: [length: 4 bytes big-endian] [CRC32: 4 bytes big-endian]
    //                [bytecode: N bytes]
    // Big-endian is written explicitly so the record is portable across
    // architectures and unambiguous on any future platform.
    const uint32_t codeLen = static_cast<uint32_t>(code.size());
    const uint32_t checksum = crc32c(code.data(), code.size());

    std::string value(CODE_HEADER_BYTES + code.size(), '\0');
    // Length — big-endian
    value[0] = (codeLen >> 24) & 0xFF;
    value[1] = (codeLen >> 16) & 0xFF;
    value[2] = (codeLen >>  8) & 0xFF;
    value[3] =  codeLen        & 0xFF;
    // CRC32 — big-endian
    value[4] = (checksum >> 24) & 0xFF;
    value[5] = (checksum >> 16) & 0xFF;
    value[6] = (checksum >>  8) & 0xFF;
    value[7] =  checksum        & 0xFF;
    if (!code.empty())
        std::memcpy(&value[CODE_HEADER_BYTES],
                    reinterpret_cast<const char *>(code.data()),
                    code.size());

    const std::string key = codeKey(addrHex);
    std::unique_lock<std::shared_mutex> lock(mutexForKey(key));
    try {
        bool ok = rocksdb.put(key, value, /*sync=*/true);
        if (!ok)
            std::cerr << "[EVMStorage] putContractCode: DB write failed for "
                      << addrHex << "\n";
        return ok;
    } catch (const std::exception &e) {
        std::cerr << "[EVMStorage] putContractCode: exception for "
                  << addrHex << ": " << e.what() << "\n";
        return false;
    }
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

    const std::string key = codeKey(addrHex);
    std::shared_lock<std::shared_mutex> lock(mutexForKey(key));

    std::string raw;
    try {
        if (!rocksdb.get(key, raw))
            return ReadStatus::NOT_FOUND;
    } catch (const std::exception &e) {
        std::cerr << "[EVMStorage] getContractCode: exception for "
                  << addrHex << ": " << e.what() << "\n";
        return ReadStatus::DB_ERROR;
    }

    if (raw.size() < CODE_HEADER_BYTES) {
        std::cerr << "[EVMStorage] getContractCode: corrupt record for "
                  << addrHex << " — header truncated ("
                  << raw.size() << " bytes)\n";
        return ReadStatus::CORRUPT;
    }

    // Decode length — big-endian explicit
    const uint32_t storedLen =
        (static_cast<uint32_t>(static_cast<uint8_t>(raw[0])) << 24) |
        (static_cast<uint32_t>(static_cast<uint8_t>(raw[1])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(raw[2])) <<  8) |
         static_cast<uint32_t>(static_cast<uint8_t>(raw[3]));

    if (raw.size() != CODE_HEADER_BYTES + storedLen) {
        std::cerr << "[EVMStorage] getContractCode: corrupt record for "
                  << addrHex << " — length prefix declares " << storedLen
                  << " bytes but body is "
                  << (raw.size() - CODE_HEADER_BYTES) << " bytes\n";
        return ReadStatus::CORRUPT;
    }

    // Decode stored CRC32 — big-endian explicit
    const uint32_t storedCRC =
        (static_cast<uint32_t>(static_cast<uint8_t>(raw[4])) << 24) |
        (static_cast<uint32_t>(static_cast<uint8_t>(raw[5])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(raw[6])) <<  8) |
         static_cast<uint32_t>(static_cast<uint8_t>(raw[7]));

    // Compute CRC over the code body and compare
    const auto *body = reinterpret_cast<const uint8_t *>(
        raw.data() + CODE_HEADER_BYTES);
    const uint32_t computedCRC = crc32c(body, storedLen);

    if (computedCRC != storedCRC) {
        std::cerr << "[EVMStorage] getContractCode: CRC mismatch for "
                  << addrHex << " — stored 0x" << std::hex << storedCRC
                  << ", computed 0x" << computedCRC << std::dec << "\n";
        return ReadStatus::CORRUPT;
    }

    codeOut.assign(body, body + storedLen);
    return ReadStatus::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Contract Storage Slots
// ─────────────────────────────────────────────────────────────────────────────

bool EVMStorage::putContractStorage(const std::string &addrHex,
                                     const std::string &slotHex,
                                     const std::string &valueHex)
{
    if (!isValidAddrHex(addrHex)) {
        std::cerr << "[EVMStorage] putContractStorage: invalid address '"
                  << addrHex << "'\n";
        return false;
    }
    if (!isValidSlotHex(slotHex)) {
        std::cerr << "[EVMStorage] putContractStorage: invalid slot '"
                  << slotHex << "' — must be exactly "
                  << SLOT_HEX_LEN << " valid hex characters\n";
        return false;
    }
    // Full character-level hex validation on the value, not just length
    if (!isValidSlotHex(valueHex)) {
        std::cerr << "[EVMStorage] putContractStorage: invalid value '"
                  << valueHex << "' — must be exactly "
                  << SLOT_HEX_LEN << " valid hex characters\n";
        return false;
    }

    const std::string key = storageKey(addrHex, slotHex);
    std::unique_lock<std::shared_mutex> lock(mutexForKey(key));
    try {
        bool ok = rocksdb.put(key, valueHex, /*sync=*/true);
        if (!ok)
            std::cerr << "[EVMStorage] putContractStorage: DB write failed for "
                      << addrHex << " slot " << slotHex << "\n";
        return ok;
    } catch (const std::exception &e) {
        std::cerr << "[EVMStorage] putContractStorage: exception for "
                  << addrHex << ": " << e.what() << "\n";
        return false;
    }
}

EVMStorage::ReadStatus EVMStorage::getContractStorage(const std::string &addrHex,
                                                       const std::string &slotHex,
                                                       std::string &valueOut)
{
    valueOut.clear();

    if (!isValidAddrHex(addrHex)) {
        std::cerr << "[EVMStorage] getContractStorage: invalid address '"
                  << addrHex << "'\n";
        return ReadStatus::DB_ERROR;
    }
    if (!isValidSlotHex(slotHex)) {
        std::cerr << "[EVMStorage] getContractStorage: invalid slot '"
                  << slotHex << "'\n";
        return ReadStatus::DB_ERROR;
    }

    const std::string key = storageKey(addrHex, slotHex);
    std::shared_lock<std::shared_mutex> lock(mutexForKey(key));

    try {
        if (!rocksdb.get(key, valueOut))
            return ReadStatus::NOT_FOUND;
    } catch (const std::exception &e) {
        std::cerr << "[EVMStorage] getContractStorage: exception for "
                  << addrHex << ": " << e.what() << "\n";
        return ReadStatus::DB_ERROR;
    }

    // Size check
    if (valueOut.size() != SLOT_HEX_LEN) {
        std::cerr << "[EVMStorage] getContractStorage: corrupt value for "
                  << addrHex << " slot " << slotHex
                  << " — size " << valueOut.size()
                  << ", expected " << SLOT_HEX_LEN << "\n";
        valueOut.clear();
        return ReadStatus::CORRUPT;
    }

    // Character-level hex validation on the value read back
    if (!isValidSlotHex(valueOut)) {
        std::cerr << "[EVMStorage] getContractStorage: non-hex data in value for "
                  << addrHex << " slot " << slotHex << "\n";
        valueOut.clear();
        return ReadStatus::CORRUPT;
    }

    return ReadStatus::OK;
}

bool EVMStorage::deleteContractStorage(const std::string &addrHex,
                                        const std::string &slotHex)
{
    if (!isValidAddrHex(addrHex)) {
        std::cerr << "[EVMStorage] deleteContractStorage: invalid address '"
                  << addrHex << "'\n";
        return false;
    }
    if (!isValidSlotHex(slotHex)) {
        std::cerr << "[EVMStorage] deleteContractStorage: invalid slot '"
                  << slotHex << "'\n";
        return false;
    }

    const std::string key = storageKey(addrHex, slotHex);
    std::unique_lock<std::shared_mutex> lock(mutexForKey(key));
    try {
        bool ok = rocksdb.del(key, /*sync=*/true);
        if (!ok)
            std::cerr << "[EVMStorage] deleteContractStorage: DB del failed for "
                      << addrHex << " slot " << slotHex << "\n";
        return ok;
    } catch (const std::exception &e) {
        std::cerr << "[EVMStorage] deleteContractStorage: exception for "
                  << addrHex << ": " << e.what() << "\n";
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Batch Storage Write
// ─────────────────────────────────────────────────────────────────────────────

bool EVMStorage::putContractStorageBatch(const std::vector<StorageBatchItem> &items)
{
    if (items.empty()) return true;

    // Full validation — including character-level hex checks — on every item
    // before touching the DB so we never write a partial batch
    for (size_t i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        if (!isValidAddrHex(item.addrHex)) {
            std::cerr << "[EVMStorage] putContractStorageBatch: invalid address '"
                      << item.addrHex << "' at index " << i << "\n";
            return false;
        }
        if (!isValidSlotHex(item.slotHex)) {
            std::cerr << "[EVMStorage] putContractStorageBatch: invalid slot '"
                      << item.slotHex << "' at index " << i << "\n";
            return false;
        }
        if (!isValidSlotHex(item.valueHex)) {
            std::cerr << "[EVMStorage] putContractStorageBatch: invalid value '"
                      << item.valueHex << "' at index " << i << "\n";
            return false;
        }
    }

    std::vector<std::pair<std::string, std::string>> kvPairs;
    kvPairs.reserve(items.size());
    for (const auto &item : items)
        kvPairs.emplace_back(storageKey(item.addrHex, item.slotHex),
                             item.valueHex);

    try {
        bool ok = rocksdb.batchPut(kvPairs, /*sync=*/true);
        if (!ok)
            std::cerr << "[EVMStorage] putContractStorageBatch: atomic write "
                         "failed (" << items.size() << " items)\n";
        return ok;
    } catch (const std::exception &e) {
        std::cerr << "[EVMStorage] putContractStorageBatch: exception: "
                  << e.what() << "\n";
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Balances — 256-bit
// ─────────────────────────────────────────────────────────────────────────────

bool EVMStorage::putBalance(const std::string &addrHex, const uint256 &balance)
{
    if (!isValidAddrHex(addrHex)) {
        std::cerr << "[EVMStorage] putBalance: invalid address '"
                  << addrHex << "'\n";
        return false;
    }

    const std::string value(reinterpret_cast<const char *>(balance.data()),
                             BALANCE_BYTES);
    const std::string key = balanceKey(addrHex);
    std::unique_lock<std::shared_mutex> lock(mutexForKey(key));
    try {
        bool ok = rocksdb.put(key, value, /*sync=*/true);
        if (!ok)
            std::cerr << "[EVMStorage] putBalance: DB write failed for "
                      << addrHex << "\n";
        return ok;
    } catch (const std::exception &e) {
        std::cerr << "[EVMStorage] putBalance: exception for "
                  << addrHex << ": " << e.what() << "\n";
        return false;
    }
}

EVMStorage::ReadStatus EVMStorage::getBalance(const std::string &addrHex,
                                               uint256 &balanceOut)
{
    balanceOut.fill(0);

    if (!isValidAddrHex(addrHex)) {
        std::cerr << "[EVMStorage] getBalance: invalid address '"
                  << addrHex << "'\n";
        return ReadStatus::DB_ERROR;
    }

    const std::string key = balanceKey(addrHex);
    std::shared_lock<std::shared_mutex> lock(mutexForKey(key));

    std::string raw;
    try {
        if (!rocksdb.get(key, raw))
            return ReadStatus::NOT_FOUND;
    } catch (const std::exception &e) {
        std::cerr << "[EVMStorage] getBalance: exception for "
                  << addrHex << ": " << e.what() << "\n";
        return ReadStatus::DB_ERROR;
    }

    if (raw.size() != BALANCE_BYTES) {
        std::cerr << "[EVMStorage] getBalance: corrupt record for "
                  << addrHex << " — size " << raw.size()
                  << ", expected " << BALANCE_BYTES << "\n";
        return ReadStatus::CORRUPT;
    }

    std::memcpy(balanceOut.data(), raw.data(), BALANCE_BYTES);
    return ReadStatus::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Balances — 64-bit convenience wrappers
// ─────────────────────────────────────────────────────────────────────────────

bool EVMStorage::putBalance64(const std::string &addrHex, uint64_t balance)
{
    uint256 buf{};
    // Encode into the last 8 bytes in big-endian; upper 24 bytes remain zero
    for (int i = 7; i >= 0; --i)
        buf[BALANCE_BYTES - 8 + (7 - i)] =
            static_cast<uint8_t>(balance >> (8 * i));
    return putBalance(addrHex, buf);
}

EVMStorage::ReadStatus EVMStorage::getBalance64(const std::string &addrHex,
                                                 uint64_t &balanceOut)
{
    balanceOut = 0;

    uint256 buf{};
    const ReadStatus status = getBalance(addrHex, buf);
    if (status != ReadStatus::OK) return status;

    // Reject values that exceed uint64 range rather than silently truncating
    for (size_t i = 0; i < BALANCE_BYTES - 8; ++i) {
        if (buf[i] != 0x00) {
            std::cerr << "[EVMStorage] getBalance64: balance for "
                      << addrHex << " exceeds uint64 range — "
                      << "call getBalance() for full 256-bit precision\n";
            return ReadStatus::CORRUPT;
        }
    }

    for (size_t i = 0; i < 8; ++i)
        balanceOut = (balanceOut << 8) | buf[BALANCE_BYTES - 8 + i];

    return ReadStatus::OK;
}
