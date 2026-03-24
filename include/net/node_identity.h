#pragma once

#include "crypto/secp256k1_wrapper.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace net {

// =============================================================================
// DOMAIN SEPARATION TAG
// Prevents replay and cross-protocol attacks.
// All signing goes through this prefix — matches Bitcoin/Ethereum convention.
// =============================================================================
static constexpr char NODE_SIGN_DOMAIN[] =
    "\x19MedorCoin Signed Node Message:\n32";

// =============================================================================
// NODE IDENTITY
// Cryptographic identity for a P2P node using secp256k1.
//
// Security guarantees:
//   - Private key zeroed from all copies immediately after use.
//   - Key file saved with 0600 permissions (owner read/write only).
//   - verify() uses constant-time comparison (no timing attacks).
//   - Signatures enforced to low-S canonical form (no malleability).
//   - All signing domain-separated to prevent cross-protocol replay.
//   - Public key validated for correct size and curve membership.
//   - No exceptions used for normal control flow.
//
// Node ID derivation:
//   Keccak-256(pubkey64)[12..31] hex-encoded — identical to Ethereum enode.
//
// Thread-safe: all public methods are const after construction.
// =============================================================================
class NodeIdentity {
public:
    // -------------------------------------------------------------------------
    // Load from existing private key file.
    // File must contain exactly one non-comment line of 64 lowercase hex chars.
    // File permissions checked — warns if world-readable.
    // Throws std::runtime_error on any failure.
    // -------------------------------------------------------------------------
    static NodeIdentity loadFromFile(const std::string& privkeyPath);

    // -------------------------------------------------------------------------
    // Generate a fresh keypair and optionally persist to file.
    // If saveToPath is non-empty the file is written with chmod 0600.
    // Throws std::runtime_error on failure.
    // -------------------------------------------------------------------------
    static NodeIdentity generate(const std::string& saveToPath = "");

    // -------------------------------------------------------------------------
    // 20-byte node ID — Keccak-256(pubkey64)[12..31], 40-char lowercase hex.
    // Matches Ethereum enode identity.
    // -------------------------------------------------------------------------
    const std::string& nodeId() const noexcept { return nodeId_; }

    // -------------------------------------------------------------------------
    // Full enode URL: enode://<128hexchars>@<host>:<port>
    // IPv6 hosts are wrapped in brackets automatically.
    // host is validated — throws on empty or obviously malformed input.
    // -------------------------------------------------------------------------
    std::string enodeUrl(const std::string& host, uint16_t port) const;

    // -------------------------------------------------------------------------
    // 64-byte uncompressed public key without 0x04 prefix.
    // Used in handshakes, ECDH, and enode URLs.
    // -------------------------------------------------------------------------
    const std::array<uint8_t, 64>& pubkey64() const noexcept {
        return pubkey64_;
    }

    // -------------------------------------------------------------------------
    // 33-byte compressed public key.
    // -------------------------------------------------------------------------
    const std::array<uint8_t, 33>& pubkeyCompressed() const noexcept {
        return keypair_.pubkey_compressed;
    }

    // -------------------------------------------------------------------------
    // Sign a 32-byte hash with domain separation.
    // Domain tag prepended before signing to prevent cross-protocol replay.
    // Signature enforced to low-S canonical form.
    // Returns nullopt on any failure — never throws.
    // -------------------------------------------------------------------------
    std::optional<crypto::Secp256k1Signature> sign(
        const std::array<uint8_t, 32>& hash32) const noexcept;

    // -------------------------------------------------------------------------
    // Verify a signature produced by sign().
    // Uses constant-time comparison — no timing side-channel.
    // Domain separation applied before verification.
    // Low-S enforced on inbound signature before accepting.
    // Returns false on any failure — never throws.
    // -------------------------------------------------------------------------
    static bool verify(
        const std::array<uint8_t, 32>&    hash32,
        const crypto::Secp256k1Signature& sig,
        const std::array<uint8_t, 64>&    expectedPubkey64) noexcept;

    // -------------------------------------------------------------------------
    // Derive 40-char hex node ID from a 64-byte pubkey.
    // Returns empty string on failure — caller must check.
    // -------------------------------------------------------------------------
    static std::string deriveNodeId(
        const std::array<uint8_t, 64>& pubkey64) noexcept;

    // -------------------------------------------------------------------------
    // Hex encode a byte array — lowercase, no prefix.
    // -------------------------------------------------------------------------
    static std::string toHex(const uint8_t* data, size_t len) noexcept;

    // -------------------------------------------------------------------------
    // Destructor zeros private key material.
    // -------------------------------------------------------------------------
    ~NodeIdentity() noexcept {
        keypair_.privkey.fill(0);
    }

    // Non-copyable — prevents accidental private key duplication.
    NodeIdentity(const NodeIdentity&)            = delete;
    NodeIdentity& operator=(const NodeIdentity&) = delete;

    // Moveable — private key moves, source zeroed.
    NodeIdentity(NodeIdentity&&) noexcept;
    NodeIdentity& operator=(NodeIdentity&&) noexcept;

private:
    NodeIdentity() = default;

    crypto::Secp256k1Keypair keypair_;
    std::array<uint8_t, 64>  pubkey64_{};
    std::string              nodeId_;

    // -------------------------------------------------------------------------
    // Apply domain separation tag + hash to produce the actual value signed.
    // Prevents replay across protocols that use the same key.
    // -------------------------------------------------------------------------
    static std::array<uint8_t, 32> domainHash(
        const std::array<uint8_t, 32>& hash32) noexcept;

    // -------------------------------------------------------------------------
    // Enforce low-S canonical form on a signature.
    // Returns false if normalization fails.
    // -------------------------------------------------------------------------
    static bool enforceCanonical(
        crypto::Secp256k1Signature& sig) noexcept;

    // -------------------------------------------------------------------------
    // Constant-time byte comparison — no timing side-channel.
    // -------------------------------------------------------------------------
    static bool constTimeEqual(const uint8_t* a,
                                const uint8_t* b,
                                size_t len) noexcept;

    friend NodeIdentity buildFromKeypair(
        const crypto::Secp256k1Keypair& kp);
};

} // namespace net
