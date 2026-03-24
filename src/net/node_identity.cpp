#include "net/node_identity.h"
#include "crypto/secp256k1_wrapper.h"
#include "crypto/keccak256.h"

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#if !defined(_WIN32)
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <fcntl.h>
#  include <unistd.h>
#else
#  include <windows.h>
#  include <aclapi.h>
#  include <sddl.h>
#  pragma comment(lib, "advapi32.lib")
#endif

namespace net {

// =============================================================================
// DOMAIN SEPARATION
// =============================================================================
static constexpr uint8_t DOMAIN_TAG[] = {
    0x19,'M','e','d','o','r','C','o','i','n',' ',
    'S','i','g','n','e','d',' ','N','o','d','e',' ',
    'M','e','s','s','a','g','e',':','\n','3','2'
};
static constexpr size_t DOMAIN_TAG_LEN = sizeof(DOMAIN_TAG);

// =============================================================================
// SECURE ZERO
// Uses memset_s where available (C11 Annex K).
// Falls back to volatile pointer write — prevents optimisation away
// on all major compilers (GCC, Clang, MSVC) with standard flags.
// For absolute guarantee link with libsodium and use
// sodium_memzero() instead.
// =============================================================================
template<size_t N>
static void secureZero(std::array<uint8_t, N>& arr) noexcept {
#if defined(__STDC_LIB_EXT1__)
    memset_s(arr.data(), N, 0, N);
#else
    volatile uint8_t* p = arr.data();
    for (size_t i = 0; i < N; i++) p[i] = 0;
#endif
}

static void secureZeroKeypair(
    crypto::Secp256k1Keypair& kp) noexcept
{
    secureZero(kp.privkey);
    secureZero(kp.pubkey_compressed);
    secureZero(kp.pubkey_uncompressed);
}

// =============================================================================
// MOVE / DESTRUCTOR
// Copy constructor and copy assignment DELETED — no key duplication.
// =============================================================================
NodeIdentity::NodeIdentity(NodeIdentity&& o) noexcept
    : keypair_(o.keypair_)
    , pubkey64_(o.pubkey64_)
    , nodeId_(std::move(o.nodeId_))
{
    secureZeroKeypair(o.keypair_);
    secureZero(o.pubkey64_);
}

NodeIdentity& NodeIdentity::operator=(NodeIdentity&& o) noexcept {
    if (this != &o) {
        secureZeroKeypair(keypair_);
        secureZero(pubkey64_);
        keypair_  = o.keypair_;
        pubkey64_ = o.pubkey64_;
        nodeId_   = std::move(o.nodeId_);
        secureZeroKeypair(o.keypair_);
        secureZero(o.pubkey64_);
    }
    return *this;
}

NodeIdentity::~NodeIdentity() noexcept {
    secureZeroKeypair(keypair_);
    secureZero(pubkey64_);
}

// =============================================================================
// toHex — null-safe
// =============================================================================
std::string NodeIdentity::toHex(const uint8_t* data,
                                 size_t len) noexcept {
    if (!data || len == 0) return {};
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(HEX[(data[i] >> 4) & 0xF]);
        out.push_back(HEX[ data[i]       & 0xF]);
    }
    return out;
}

// =============================================================================
// CONSTANT-TIME COMPARISON
// =============================================================================
bool NodeIdentity::constTimeEqual(const uint8_t* a,
                                   const uint8_t* b,
                                   size_t len) noexcept {
    if (!a || !b || len == 0) return false;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++)
        diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

// =============================================================================
// DOMAIN HASH
// Never falls back to original hash on failure.
// Returns zeroed array and sets ok=false on any failure.
// =============================================================================
std::array<uint8_t, 32> NodeIdentity::domainHash(
    const std::array<uint8_t, 32>& hash32,
    bool& ok) noexcept
{
    ok = false;
    std::array<uint8_t, 32> zero{};
    try {
        std::vector<uint8_t> msg;
        msg.reserve(DOMAIN_TAG_LEN + 32);
        for (size_t i = 0; i < DOMAIN_TAG_LEN; i++)
            msg.push_back(DOMAIN_TAG[i]);
        for (auto b : hash32)
            msg.push_back(b);
        auto result = keccak256(msg.data(), msg.size());
        if (result.size() != 32) return zero;
        std::array<uint8_t, 32> out{};
        std::memcpy(out.data(), result.data(), 32);
        ok = true;
        return out;
    } catch (...) {
        return zero;
    }
}

// =============================================================================
// ENFORCE CANONICAL SIGNATURE (low-S)
// Uses libsecp256k1 normalize — no manual big-int math.
// Validates r and s non-zero before normalisation.
// Validates recid is 0 or 1 after normalisation.
// =============================================================================
bool NodeIdentity::enforceCanonical(
    crypto::Secp256k1Signature& sig) noexcept
{
    // Validate recid bounds FIRST
    if (sig.recid < 0 || sig.recid > 1) return false;

    // Reject zero r or zero s
    bool rZero = true, sZero = true;
    for (int i = 0; i < 32; i++) {
        if (sig.r[i] != 0) rZero = false;
        if (sig.s[i] != 0) sZero = false;
    }
    if (rZero || sZero) return false;

    secp256k1_context* ctx = nullptr;
    try { ctx = crypto::getCtx(); }
    catch (...) { return false; }

    uint8_t compact[64]{};
    std::memcpy(compact,      sig.r.data(), 32);
    std::memcpy(compact + 32, sig.s.data(), 32);

    secp256k1_ecdsa_signature libsig{};
    if (secp256k1_ecdsa_signature_parse_compact(
            ctx, &libsig, compact) != 1)
        return false;

    int normalised = secp256k1_ecdsa_signature_normalize(
        ctx, &libsig, &libsig);

    uint8_t normCompact[64]{};
    if (secp256k1_ecdsa_signature_serialize_compact(
            ctx, normCompact, &libsig) != 1)
        return false;

    std::memcpy(sig.r.data(), normCompact,      32);
    std::memcpy(sig.s.data(), normCompact + 32, 32);

    if (normalised) sig.recid ^= 1;

    // Final recid bounds check after flip
    if (sig.recid < 0 || sig.recid > 1) return false;

    return true;
}

// =============================================================================
// FILE WRITE — POSIX
// O_CREAT with mode 0600 from creation.
// fsync before rename — crash/power-loss safe.
// =============================================================================
#if !defined(_WIN32)
static void writeKeyFilePosix(const std::string& content,
                               const std::string& path)
{
    std::string tmp = path + ".tmp";

    int fd = ::open(tmp.c_str(),
                    O_CREAT | O_WRONLY | O_TRUNC,
                    S_IRUSR | S_IWUSR); // 0600 from creation
    if (fd < 0)
        throw std::runtime_error(
            "[NodeIdentity] cannot create tmp key file: " + tmp);

    const char* ptr  = content.data();
    size_t      left = content.size();
    while (left > 0) {
        ssize_t n = ::write(fd, ptr, left);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            throw std::runtime_error(
                "[NodeIdentity] write failed: " + tmp);
        }
        ptr  += n;
        left -= static_cast<size_t>(n);
    }

    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        throw std::runtime_error(
            "[NodeIdentity] fsync failed: " + tmp);
    }
    ::close(fd);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        throw std::runtime_error(
            "[NodeIdentity] rename failed: "
            + tmp + " -> " + path);
    }
}

static void checkFilePermissions(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) {
        if (st.st_mode & (S_IRGRP | S_IWGRP |
                           S_IROTH | S_IWOTH))
            throw std::runtime_error(
                "[NodeIdentity] SECURITY: key file is "
                "group/world readable: " + path
                + " — run: chmod 600 " + path);
    }
}

// =============================================================================
// =============================================================================
// FILE WRITE — WINDOWS
// ACL set to CURRENT USER only via OpenProcessToken + GetTokenInformation.
// PROTECTED_DACL_SECURITY_INFORMATION used to block inheritance.
// SE_DACL_PROTECTED flag set in security descriptor to prevent
// inherited ACEs from parent directory from applying.
// =============================================================================
#else
static void writeKeyFileWindows(const std::string& content,
                                 const std::string& path)
{
    std::string tmp = path + ".tmp";

    // Get current user SID from process token
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_QUERY, &hToken))
        throw std::runtime_error(
            "[NodeIdentity] OpenProcessToken failed");

    DWORD tokenInfoLen = 0;
    GetTokenInformation(hToken, TokenUser,
                        nullptr, 0, &tokenInfoLen);

    std::vector<uint8_t> tokenInfoBuf(tokenInfoLen);
    if (!GetTokenInformation(hToken, TokenUser,
                              tokenInfoBuf.data(),
                              tokenInfoLen,
                              &tokenInfoLen)) {
        CloseHandle(hToken);
        throw std::runtime_error(
            "[NodeIdentity] GetTokenInformation failed");
    }
    CloseHandle(hToken);

    auto* pTokenUser = reinterpret_cast<TOKEN_USER*>(
        tokenInfoBuf.data());
    PSID pCurrentUserSid = pTokenUser->User.Sid;

    // Write file
    HANDLE hFile = CreateFileA(
        tmp.c_str(),
        GENERIC_WRITE,
        0,         // no sharing
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
        throw std::runtime_error(
            "[NodeIdentity] CreateFile failed: " + tmp);

    DWORD written = 0;
    BOOL writeOk  = WriteFile(
        hFile,
        content.data(),
        static_cast<DWORD>(content.size()),
        &written, nullptr);

    if (!writeOk ||
        written != static_cast<DWORD>(content.size())) {
        CloseHandle(hFile);
        DeleteFileA(tmp.c_str());
        throw std::runtime_error(
            "[NodeIdentity] WriteFile failed: " + tmp);
    }

    if (!FlushFileBuffers(hFile)) {
        CloseHandle(hFile);
        DeleteFileA(tmp.c_str());
        throw std::runtime_error(
            "[NodeIdentity] FlushFileBuffers failed: " + tmp);
    }
    CloseHandle(hFile);

    // Build ACL — current user only, read + write
    EXPLICIT_ACCESSA ea{};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = NO_INHERITANCE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName    =
        reinterpret_cast<LPSTR>(pCurrentUserSid);

    PACL pAcl = nullptr;
    if (SetEntriesInAclA(1, &ea, nullptr, &pAcl) != ERROR_SUCCESS
        || pAcl == nullptr) {
        DeleteFileA(tmp.c_str());
        throw std::runtime_error(
            "[NodeIdentity] SetEntriesInAcl failed: " + tmp);
    }

    // Build a new security descriptor with SE_DACL_PROTECTED set.
    // This explicitly blocks all inherited ACEs from the parent
    // directory — no inherited permissions can apply to this file.
    SECURITY_DESCRIPTOR sd{};
    if (!InitializeSecurityDescriptor(
            &sd, SECURITY_DESCRIPTOR_REVISION)) {
        LocalFree(pAcl);
        DeleteFileA(tmp.c_str());
        throw std::runtime_error(
            "[NodeIdentity] InitializeSecurityDescriptor failed");
    }

    // SetSecurityDescriptorDacl with bDaclPresent=TRUE and
    // bDaclDefaulted=FALSE — explicit DACL, not inherited default
    if (!SetSecurityDescriptorDacl(&sd, TRUE, pAcl, FALSE)) {
        LocalFree(pAcl);
        DeleteFileA(tmp.c_str());
        throw std::runtime_error(
            "[NodeIdentity] SetSecurityDescriptorDacl failed");
    }

    // Apply with both DACL_SECURITY_INFORMATION and
    // PROTECTED_DACL_SECURITY_INFORMATION.
    // PROTECTED_DACL_SECURITY_INFORMATION sets SE_DACL_PROTECTED
    // on the object — this is what actually blocks inheritance.
    DWORD aclResult = SetNamedSecurityInfoA(
        const_cast<char*>(tmp.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION
            | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,   // owner — not changing
        nullptr,   // group — not changing
        pAcl,
        nullptr);  // SACL — not changing

    LocalFree(pAcl);

    if (aclResult != ERROR_SUCCESS) {
        DeleteFileA(tmp.c_str());
        throw std::runtime_error(
            "[NodeIdentity] SetNamedSecurityInfo failed: "
            + tmp + " error="
            + std::to_string(aclResult));
    }

    // Atomic move — replaces destination if it exists
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        throw std::runtime_error(
            "[NodeIdentity] MoveFileEx failed: "
            + tmp + " -> " + path);
    }
}
#endif

static void savePrivkeyToFile(
    const std::array<uint8_t, 32>& key,
    const std::string& path)
{
    std::string content;
    content += "# MedorCoin node private key\n";
    content += "# DO NOT share or commit this file.\n";
    content += "# Add to .gitignore: " + path + "\n";
    content += NodeIdentity::toHex(key.data(), 32) + "\n";

#if !defined(_WIN32)
    writeKeyFilePosix(content, path);
#else
    writeKeyFileWindows(content, path);
#endif
}

// =============================================================================
// PRIVATE KEY FILE LOADING
// =============================================================================
static std::array<uint8_t, 32> loadPrivkeyFromFile(
    const std::string& path)
{
#if !defined(_WIN32)
    checkFilePermissions(path);
#endif

    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error(
            "[NodeIdentity] cannot open key file: " + path);

    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() &&
               (static_cast<unsigned char>(
                    line.front()) == 0xEF          ||
                line.front() == '\r' ||
                line.front() == '\n' ||
                line.front() == ' '  ||
                line.front() == '\t'))
            line.erase(line.begin());
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == '\n' ||
                line.back() == ' '  || line.back() == '\t'))
            line.pop_back();

        if (line.empty() || line[0] == '#') continue;

        if (line.size() != 64)
            throw std::runtime_error(
                "[NodeIdentity] key line must be 64 hex chars: "
                + path + " (got "
                + std::to_string(line.size()) + ")");

        for (char c : line) {
            if (!((c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F')))
                throw std::runtime_error(
                    "[NodeIdentity] invalid hex char '"
                    + std::string(1, c)
                    + "' in key file: " + path);
        }

        std::array<uint8_t, 32> key{};
        for (size_t i = 0; i < 32; i++) {
            unsigned int byte = 0;
            std::istringstream ss(line.substr(i * 2, 2));
            ss >> std::hex >> byte;
            key[i] = static_cast<uint8_t>(byte);
        }
        line.assign(line.size(), '0');
        return key;
    }
    throw std::runtime_error(
        "[NodeIdentity] no valid key line found in: " + path);
}

// =============================================================================
// DERIVE NODE ID
// =============================================================================
std::string NodeIdentity::deriveNodeId(
    const std::array<uint8_t, 64>& pubkey64) noexcept
{
    try {
        auto hash = keccak256(pubkey64.data(), 64);
        if (hash.size() != 32) return {};
        return toHex(hash.data() + 12, 20);
    } catch (...) {
        return {};
    }
}

// =============================================================================
// BUILD FROM KEYPAIR — takes by value, zeros local copy
// =============================================================================
static NodeIdentity buildFromKeypair(
    crypto::Secp256k1Keypair kp)
{
    if (kp.pubkey_compressed[0] != 0x02 &&
        kp.pubkey_compressed[0] != 0x03) {
        secureZeroKeypair(kp);
        throw std::runtime_error(
            "[NodeIdentity] compressed pubkey invalid prefix");
    }

    if (kp.pubkey_uncompressed[0] != 0x04) {
        secureZeroKeypair(kp);
        throw std::runtime_error(
            "[NodeIdentity] uncompressed pubkey missing 0x04");
    }

    NodeIdentity id;
    id.keypair_ = kp;
    secureZeroKeypair(kp);

    std::memcpy(id.pubkey64_.data(),
                id.keypair_.pubkey_uncompressed.data() + 1,
                64);

    id.nodeId_ = NodeIdentity::deriveNodeId(id.pubkey64_);
    if (id.nodeId_.empty()) {
        secureZeroKeypair(id.keypair_);
        secureZero(id.pubkey64_);
        throw std::runtime_error(
            "[NodeIdentity] node ID derivation failed");
    }

    return id;
}

// =============================================================================
// LOAD FROM FILE
// =============================================================================
NodeIdentity NodeIdentity::loadFromFile(
    const std::string& privkeyPath)
{
    auto privkey = loadPrivkeyFromFile(privkeyPath);
    auto* ctx    = crypto::getCtx();

    if (secp256k1_ec_seckey_verify(
            ctx, privkey.data()) != 1) {
        secureZero(privkey);
        throw std::runtime_error(
            "[NodeIdentity] private key fails curve validation: "
            + privkeyPath);
    }

    secp256k1_pubkey pub{};
    if (secp256k1_ec_pubkey_create(
            ctx, &pub, privkey.data()) != 1) {
        secureZero(privkey);
        throw std::runtime_error(
            "[NodeIdentity] pubkey derivation failed");
    }

    crypto::Secp256k1Keypair kp{};
    std::memcpy(kp.privkey.data(), privkey.data(), 32);
    secureZero(privkey);

    size_t compLen = 33;
    if (secp256k1_ec_pubkey_serialize(
            ctx, kp.pubkey_compressed.data(),
            &compLen, &pub,
            SECP256K1_EC_COMPRESSED) != 1
        || compLen != 33) {
        secureZeroKeypair(kp);
        throw std::runtime_error(
            "[NodeIdentity] compressed pubkey serialization failed");
    }

    size_t uncompLen = 65;
    if (secp256k1_ec_pubkey_serialize(
            ctx, kp.pubkey_uncompressed.data(),
            &uncompLen, &pub,
            SECP256K1_EC_UNCOMPRESSED) != 1
        || uncompLen != 65) {
        secureZeroKeypair(kp);
        throw std::runtime_error(
            "[NodeIdentity] uncompressed pubkey serialization "
            "failed");
    }

    return buildFromKeypair(kp);
}

// =============================================================================
// GENERATE
// =============================================================================
NodeIdentity NodeIdentity::generate(
    const std::string& saveToPath)
{
    auto kpOpt = crypto::generateKeypair();
    if (!kpOpt)
        throw std::runtime_error(
            "[NodeIdentity] keypair generation failed");

    auto* ctx = crypto::getCtx();
    if (secp256k1_ec_seckey_verify(
            ctx, kpOpt->privkey.data()) != 1) {
        secureZeroKeypair(*kpOpt);
        throw std::runtime_error(
            "[NodeIdentity] generated key failed curve validation");
    }

    if (!saveToPath.empty()) {
        try {
            savePrivkeyToFile(kpOpt->privkey, saveToPath);
        } catch (...) {
            secureZeroKeypair(*kpOpt);
            throw;
        }
    }

    return buildFromKeypair(*kpOpt);
}

// =============================================================================
// ENODE URL
// =============================================================================
std::string NodeIdentity::enodeUrl(const std::string& host,
                                    uint16_t port) const
{
    if (host.empty())
        throw std::runtime_error(
            "[NodeIdentity] enodeUrl: host must not be empty");

    for (char c : host) {
        if (static_cast<unsigned char>(c) < 0x21 ||
            static_cast<unsigned char>(c) > 0x7E)
            throw std::runtime_error(
                "[NodeIdentity] enodeUrl: invalid host character");
    }

    std::string h = host;
    if (h.find(':') != std::string::npos &&
        h.front() != '[')
        h = "[" + h + "]";

    return "enode://"
         + toHex(pubkey64_.data(), 64)
         + "@" + h
         + ":" + std::to_string(port);
}

// =============================================================================
// SIGN
// =============================================================================
std::optional<crypto::Secp256k1Signature> NodeIdentity::sign(
    const std::array<uint8_t, 32>& hash32) const noexcept
{
    bool ok = false;
    auto tagged = domainHash(hash32, ok);
    if (!ok) return std::nullopt;

    auto sig = crypto::signRecoverable(
        std::span<const uint8_t, 32>(tagged.data(), 32),
        std::span<const uint8_t, 32>(
            keypair_.privkey.data(), 32));

    if (!sig) return std::nullopt;
    if (!enforceCanonical(*sig)) return std::nullopt;

    return sig;
}

// =============================================================================
// VERIFY
// Validates expectedPubkey64 non-zero.
// Validates recovered key is valid curve point.
// Validates recovered size is exactly 65 bytes.
// Constant-time comparison.
// =============================================================================
bool NodeIdentity::verify(
    const std::array<uint8_t, 32>&    hash32,
    const crypto::Secp256k1Signature& sig,
    const std::array<uint8_t, 64>&    expectedPubkey64) noexcept
{
    // Reject all-zero expected pubkey
    bool allZero = true;
    for (auto b : expectedPubkey64)
        if (b != 0) { allZero = false; break; }
    if (allZero) return false;

    crypto::Secp256k1Signature canonical = sig;
    if (!enforceCanonical(canonical)) return false;

    bool ok = false;
    auto tagged = domainHash(hash32, ok);
    if (!ok) return false;

    auto recovered = crypto::recoverPubkeyUncompressed(
        std::span<const uint8_t, 32>(tagged.data(), 32),
        canonical);
    if (!recovered) return false;

    // Validate recovered key size is exactly 65 bytes
    if (recovered->size() != 65) return false;

    // Validate recovered key is a valid curve point
    secp256k1_context* ctx = nullptr;
    try { ctx = crypto::getCtx(); }
    catch (...) { return false; }

    secp256k1_pubkey pub{};
    if (secp256k1_ec_pubkey_parse(
            ctx, &pub,
            recovered->data(),
            65) != 1)
        return false;

    // Constant-time compare — skip 0x04 prefix byte
    return constTimeEqual(
        recovered->data() + 1,
        expectedPubkey64.data(), 64);
}

} // namespace net
