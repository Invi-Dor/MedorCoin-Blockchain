#include "evm/host.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// Static helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string MedorEVMHost::hexEncode(const uint8_t *data, size_t len) noexcept
{
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(HEX[data[i] >> 4]);
        out.push_back(HEX[data[i] & 0x0F]);
    }
    return out;
}

// Fills the last 8 bytes of a 32-byte big-endian word with value.
// The upper 24 bytes remain zero, consistent with EVM uint256 semantics
// where a native uint64 balance lives in the least-significant 8 bytes.
evmc::uint256be MedorEVMHost::uint64ToUint256be(uint64_t value) noexcept
{
    evmc::uint256be out{};
    // bytes[24..31] are the least-significant 8 bytes in big-endian order
    for (int i = 0; i < 8; ++i)
        out.bytes[24 + i] = static_cast<uint8_t>(value >> (56 - 8 * i));
    return out;
}

bool MedorEVMHost::isAllZero(const evmc_bytes32 &v) noexcept
{
    for (auto b : v.bytes) if (b) return false;
    return true;
}

// Decodes exactly 64 valid hex characters into a 32-byte evmc_bytes32.
// Validates every character before writing so a corrupt storage value
// can never silently produce a wrong result.
bool MedorEVMHost::hexToBytes32(const std::string &hex,
                                  evmc_bytes32      &out) noexcept
{
    if (hex.size() != 64) return false;

    for (size_t i = 0; i < 32; ++i) {
        const char hi = static_cast<char>(
            std::tolower(static_cast<unsigned char>(hex[i * 2])));
        const char lo = static_cast<char>(
            std::tolower(static_cast<unsigned char>(hex[i * 2 + 1])));

        if (!std::isxdigit(static_cast<unsigned char>(hi)) ||
            !std::isxdigit(static_cast<unsigned char>(lo)))
            return false;

        const uint8_t hiByte = std::isdigit(static_cast<unsigned char>(hi))
                               ? static_cast<uint8_t>(hi - '0')
                               : static_cast<uint8_t>(hi - 'a' + 10);
        const uint8_t loByte = std::isdigit(static_cast<unsigned char>(lo))
                               ? static_cast<uint8_t>(lo - '0')
                               : static_cast<uint8_t>(lo - 'a' + 10);

        out.bytes[i] = static_cast<uint8_t>((hiByte << 4) | loByte);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Hex cache — assumes stateMutex is already held by caller
// ─────────────────────────────────────────────────────────────────────────────

const std::string &MedorEVMHost::cachedAddrHex(
    const evmc::address &addr) const noexcept
{
    auto it = addrHexCache.find(addr);
    if (it != addrHexCache.end()) return it->second;
    return addrHexCache
        .emplace(addr, hexEncode(addr.bytes, sizeof(addr.bytes)))
        .first->second;
}

const std::string &MedorEVMHost::cachedKeyHex(
    const evmc_bytes32 &key) const noexcept
{
    auto it = keyHexCache.find(key);
    if (it != keyHexCache.end()) return it->second;
    return keyHexCache
        .emplace(key, hexEncode(key.bytes, sizeof(key.bytes)))
        .first->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

MedorEVMHost::MedorEVMHost(EVMStorage &storage)
    : storageDB(storage)
{
}

// ─────────────────────────────────────────────────────────────────────────────
// Transaction frame management
// ─────────────────────────────────────────────────────────────────────────────

void MedorEVMHost::setTxContext(const evmc_tx_context &ctx) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);
    txContext = ctx;
}

void MedorEVMHost::beginTransaction() noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);
    logs.clear();
    gas                  = GasUsage{};
    storageErrorOccurred = false;
    inSelfdestruct       = false;
    accessedAccounts.clear();
    accessedStorageSlots.clear();
    addrHexCache.clear();
    keyHexCache.clear();
}

void MedorEVMHost::commitTransaction() noexcept
{
    // Access lists are intentionally preserved until beginTransaction()
    // clears them so post-execution inspection remains possible.
}

// ─────────────────────────────────────────────────────────────────────────────
// Post-execution inspection — return copies so callers hold no raw references
// ─────────────────────────────────────────────────────────────────────────────

std::vector<MedorEVMHost::LogEntry> MedorEVMHost::getLogs() const noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return logs;
}

MedorEVMHost::GasUsage MedorEVMHost::getGasUsage() const noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return gas;
}

bool MedorEVMHost::hadStorageError() const noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return storageErrorOccurred;
}

// ─────────────────────────────────────────────────────────────────────────────
// Account existence
// ─────────────────────────────────────────────────────────────────────────────

bool MedorEVMHost::account_exists(evmc::address addr) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);
    const std::string &key = cachedAddrHex(addr);

    EVMStorage::uint256 bal{};
    if (storageDB.getBalance(key, bal) == EVMStorage::ReadStatus::OK)
        for (auto b : bal) if (b) return true;

    std::vector<uint8_t> code;
    return storageDB.getContractCode(key, code) == EVMStorage::ReadStatus::OK
           && !code.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Storage — SLOAD
// ─────────────────────────────────────────────────────────────────────────────

evmc_bytes32 MedorEVMHost::get_storage(evmc::address addr,
                                        evmc_bytes32  key) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);

    const bool warm = accessedStorageSlots[addr].count(key) > 0;
    gas.storageReads++;
    gas.total += warm ? GAS_SLOAD_WARM : GAS_SLOAD_COLD;
    accessedStorageSlots[addr].insert(key);

    evmc_bytes32 out{};
    std::string  valueHex;

    EVMStorage::ReadStatus s = storageDB.getContractStorage(
        cachedAddrHex(addr), cachedKeyHex(key), valueHex);

    if (s == EVMStorage::ReadStatus::OK) {
        // Validate and decode — hexToBytes32 rejects non-hex or wrong-length
        // values so a corrupt record can never silently produce a wrong result
        if (!hexToBytes32(valueHex, out)) {
            std::cerr << "[MedorEVMHost] get_storage: invalid hex value in DB "
                         "for slot " << cachedKeyHex(key)
                      << " — returning zero\n";
            storageErrorOccurred = true;
            out = evmc_bytes32{};
        }
    }
    // NOT_FOUND is normal — EVM slot defaults to zero
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Storage — SSTORE
// ─────────────────────────────────────────────────────────────────────────────

evmc_storage_status MedorEVMHost::set_storage(evmc::address addr,
                                               evmc_bytes32  key,
                                               evmc_bytes32  val) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);

    const std::string &addrHex = cachedAddrHex(addr);
    const std::string &keyHex  = cachedKeyHex(key);

    std::string currentHex;
    EVMStorage::ReadStatus rs =
        storageDB.getContractStorage(addrHex, keyHex, currentHex);

    const bool currentEmpty = (rs == EVMStorage::ReadStatus::NOT_FOUND);
    const bool newIsZero    = isAllZero(val);

    const bool warm = accessedStorageSlots[addr].count(key) > 0;
    accessedStorageSlots[addr].insert(key);
    gas.storageWrites++;
    gas.total += warm ? GAS_SSTORE_WARM
               : (currentEmpty && !newIsZero ? GAS_SSTORE_SET
                                             : GAS_SSTORE_RESET);

    const std::string newValHex = hexEncode(val.bytes, sizeof(val.bytes));

    if (!storageDB.putContractStorage(addrHex, keyHex, newValHex)) {
        storageErrorOccurred = true;
        std::cerr << "[MedorEVMHost] set_storage: DB write failed for "
                  << addrHex << " slot " << keyHex << "\n";
        return EVMC_STORAGE_UNCHANGED;
    }

    if ( currentEmpty && !newIsZero)         return EVMC_STORAGE_ADDED;
    if (!currentEmpty &&  newIsZero)         return EVMC_STORAGE_DELETED;
    if (!currentEmpty && currentHex == newValHex) return EVMC_STORAGE_UNCHANGED;
    return EVMC_STORAGE_MODIFIED;
}

// ─────────────────────────────────────────────────────────────────────────────
// Balance
// ─────────────────────────────────────────────────────────────────────────────

evmc::uint256be MedorEVMHost::get_balance(evmc::address addr) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);

    const bool warm = accessedAccounts.count(addr) > 0;
    gas.balanceReads++;
    gas.total += warm ? GAS_BALANCE_WARM : GAS_BALANCE_COLD;
    accessedAccounts.insert(addr);

    EVMStorage::uint256 bal{};
    evmc::uint256be     out{};

    if (storageDB.getBalance(cachedAddrHex(addr), bal)
        == EVMStorage::ReadStatus::OK)
    {
        // EVMStorage stores balances as 32-byte big-endian — copy directly
        static_assert(sizeof(out.bytes) == 32, "evmc uint256be must be 32 bytes");
        std::memcpy(out.bytes, bal.data(), 32);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Contract code
// ─────────────────────────────────────────────────────────────────────────────

size_t MedorEVMHost::get_code_size(evmc::address addr) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);

    const bool warm = accessedAccounts.count(addr) > 0;
    gas.codeReads++;
    gas.total += warm ? GAS_EXTCODE_WARM : GAS_EXTCODE_COLD;
    accessedAccounts.insert(addr);

    std::vector<uint8_t> code;
    return storageDB.getContractCode(cachedAddrHex(addr), code)
           == EVMStorage::ReadStatus::OK
           ? code.size() : 0;
}

evmc_bytes32 MedorEVMHost::get_code_hash(evmc::address addr) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);

    evmc_bytes32 hash{};
    std::vector<uint8_t> code;

    if (storageDB.getContractCode(cachedAddrHex(addr), code)
        != EVMStorage::ReadStatus::OK || code.empty())
        return hash;

    const std::vector<uint8_t> digest = crypto::Keccak256(code);
    if (digest.size() == sizeof(hash.bytes))
        std::memcpy(hash.bytes, digest.data(), sizeof(hash.bytes));
    return hash;
}

size_t MedorEVMHost::copy_code(evmc::address addr,
                                size_t        code_offset,
                                uint8_t      *buffer_data,
                                size_t        buffer_size) noexcept
{
    if (!buffer_data || buffer_size == 0) return 0;

    std::lock_guard<std::mutex> lock(stateMutex);

    std::vector<uint8_t> code;
    if (storageDB.getContractCode(cachedAddrHex(addr), code)
        != EVMStorage::ReadStatus::OK || code_offset >= code.size())
        return 0;

    const size_t n = std::min(buffer_size, code.size() - code_offset);
    std::memcpy(buffer_data, code.data() + code_offset, n);
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
// Self-destruct — reentrancy guard + 256-bit balance transfer
// ─────────────────────────────────────────────────────────────────────────────

bool MedorEVMHost::selfdestruct(evmc::address addr,
                                 evmc::address beneficiary) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);

    if (inSelfdestruct) {
        std::cerr << "[MedorEVMHost] selfdestruct: reentrancy detected — "
                     "aborting\n";
        return false;
    }
    inSelfdestruct = true;

    gas.selfdestructs++;
    gas.total += GAS_SELFDESTRUCT;

    const std::string &addrHex = cachedAddrHex(addr);
    const std::string &benHex  = cachedAddrHex(beneficiary);

    EVMStorage::uint256 addrBal{};
    bool anyBalance = false;
    if (storageDB.getBalance(addrHex, addrBal) == EVMStorage::ReadStatus::OK)
        for (auto b : addrBal) if (b) { anyBalance = true; break; }

    if (anyBalance) {
        EVMStorage::uint256 benBal{};
        storageDB.getBalance(benHex, benBal);

        // 256-bit big-endian addition with carry propagation.
        // EVMStorage::putBalance stores big-endian so this is consistent.
        uint16_t carry = 0;
        for (int i = 31; i >= 0; --i) {
            const uint16_t sum =
                static_cast<uint16_t>(addrBal[i]) +
                static_cast<uint16_t>(benBal[i])  +
                carry;
            benBal[i] = static_cast<uint8_t>(sum & 0xFF);
            carry      = sum >> 8;
        }
        // carry != 0 here would indicate a 256-bit overflow — impossible in
        // a correctly operating chain but logged for forensic purposes
        if (carry != 0)
            std::cerr << "[MedorEVMHost] selfdestruct: 256-bit balance "
                         "overflow detected — this indicates a consensus "
                         "bug\n";

        const EVMStorage::uint256 zero{};
        const bool ok = storageDB.putBalance(benHex,  benBal)
                     && storageDB.putBalance(addrHex, zero);
        if (!ok) {
            storageErrorOccurred = true;
            std::cerr << "[MedorEVMHost] selfdestruct: balance transfer "
                         "DB write failed\n";
            inSelfdestruct = false;
            return false;
        }
    }

    inSelfdestruct = false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Transaction context and block hash
// ─────────────────────────────────────────────────────────────────────────────

evmc_tx_context MedorEVMHost::get_tx_context() noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return txContext;
}

evmc_bytes32 MedorEVMHost::get_block_hash(int64_t block_number) noexcept
{
    if (blockHashFn) {
        try { return blockHashFn(block_number); }
        catch (...) {
            std::cerr << "[MedorEVMHost] get_block_hash: resolver threw for "
                         "block " << block_number << "\n";
        }
    } else {
        std::cerr << "[MedorEVMHost] get_block_hash: no resolver registered — "
                     "returning zero hash for block " << block_number << "\n";
    }
    return evmc_bytes32{};
}

// ─────────────────────────────────────────────────────────────────────────────
// Event logging — in-memory persistence and optional durable sink
// ─────────────────────────────────────────────────────────────────────────────

void MedorEVMHost::emit_log(evmc::address        addr,
                             const uint8_t       *data,
                             size_t               data_size,
                             const evmc::bytes32  topics[],
                             size_t               topics_count) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);

    LogEntry entry;
    entry.contractHex = cachedAddrHex(addr);

    // topics_count alone guards the loop — a null topics pointer with a
    // non-zero count would be a caller contract violation, so we guard it
    // explicitly to prevent undefined behaviour.
    if (topics) {
        for (size_t i = 0; i < topics_count; ++i) {
            std::array<uint8_t, 32> t{};
            std::memcpy(t.data(), topics[i].bytes, 32);
            entry.topics.push_back(t);
        }
    }

    if (data && data_size > 0)
        entry.data.assign(data, data + data_size);

    logs.push_back(entry);

    if (logSinkFn) {
        try { logSinkFn(entry); }
        catch (...) {
            std::cerr << "[MedorEVMHost] emit_log: log sink threw — "
                         "entry is still in memory log\n";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// EIP-2929 access tracking
// ─────────────────────────────────────────────────────────────────────────────

evmc_access_status MedorEVMHost::access_account(evmc::address addr) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (accessedAccounts.count(addr)) return EVMC_ACCESS_WARM;
    accessedAccounts.insert(addr);
    return EVMC_ACCESS_COLD;
}

evmc_access_status MedorEVMHost::access_storage(evmc::address addr,
                                                  evmc_bytes32  key) noexcept
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto &slotSet = accessedStorageSlots[addr];
    if (slotSet.count(key)) return EVMC_ACCESS_WARM;
    slotSet.insert(key);
    return EVMC_ACCESS_COLD;
}
