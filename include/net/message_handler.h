#pragma once

#include "block.h"
#include "transaction.h"
#include "blockchain.h"
#include "mempool/mempool.h"
#include "net/peer_manager.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace net {

// =============================================================================
// WIRE MESSAGE TYPES
// =============================================================================
enum class MsgType : uint8_t {
    Handshake    = 0x01,
    HandshakeAck = 0x02,
    Ping         = 0x03,
    Pong         = 0x04,
    GetPeers     = 0x05,
    Peers        = 0x06,
    Inv          = 0x07,
    GetData      = 0x08,
    Block        = 0x10,
    Transaction  = 0x11,
    TxBatch      = 0x12,
    GetHeaders   = 0x20,
    Headers      = 0x21,
    GetBlocks    = 0x22,
    Reject       = 0xFE,
    Unknown      = 0xFF
};

// =============================================================================
// WIRE FRAME
// Header layout (9 bytes):
//   [0..3]  magic (4 bytes)
//   [4]     MsgType (1 byte)
//   [5..8]  payload length big-endian uint32
// =============================================================================
static constexpr size_t   FRAME_HEADER_SIZE = 9;
static constexpr uint32_t MAX_PAYLOAD_SIZE  = 8 * 1024 * 1024; // 8 MB

struct WireFrame {
    MsgType              type    = MsgType::Unknown;
    std::vector<uint8_t> payload;
};

// Structured decode error — callers react precisely per error code
enum class FrameDecodeError : uint8_t {
    None           = 0,
    TooShort       = 1,
    BadMagic       = 2,
    OversizedFrame = 3,
    PayloadTrunc   = 4,
    BadMsgType     = 5,
    AllocFailed    = 6,
    Unknown        = 255
};

struct FrameDecodeResult {
    bool             ok    = false;
    FrameDecodeError error = FrameDecodeError::None;

    const char* errorName() const noexcept {
        switch (error) {
            case FrameDecodeError::None:           return "None";
            case FrameDecodeError::TooShort:       return "TooShort";
            case FrameDecodeError::BadMagic:       return "BadMagic";
            case FrameDecodeError::OversizedFrame: return "OversizedFrame";
            case FrameDecodeError::PayloadTrunc:   return "PayloadTrunc";
            case FrameDecodeError::BadMsgType:     return "BadMsgType";
            case FrameDecodeError::AllocFailed:    return "AllocFailed";
            default:                               return "Unknown";
        }
    }
};

// =============================================================================
// MESSAGE HANDLER
// Encodes, decodes, validates, and dispatches all P2P wire messages.
// Integrates with Blockchain and Mempool for validation before dispatch.
//
// Thread safety:
//   - All callbacks and internal state protected by per-type mutexes.
//   - No global lock — send, broadcast, block, and tx callbacks
//     each have their own mutex to prevent cross-callback blocking.
//   - All user callbacks wrapped in try/catch; exceptions forwarded
//     to logFn_ and never propagate into the networking loop.
//   - handleRaw() is safe to call from multiple threads concurrently.
//
// Integration:
//   - Blockchain: used to validate inbound blocks before forwarding.
//   - Mempool: used to validate inbound transactions before forwarding.
//   - PeerManager: rate-limit check, penalty, reward, and ban on every frame.
// =============================================================================
class MessageHandler {
public:
    struct Config {
        std::string networkMagic    = "MEDOR";
        uint32_t    protocolVersion = 1;
        uint32_t    minPeerVersion  = 1;
        uint32_t    maxPeerVersion  = 1;
        std::string nodeId;
        size_t      maxPayloadBytes = MAX_PAYLOAD_SIZE;
        // Payload max enforced on all inbound frames for DoS prevention
    };

    // Callbacks — all invoked without any internal mutex held
    using SendFn      = std::function<void(const std::string& peerId,
                                            std::vector<uint8_t>)>;
    using BroadcastFn = std::function<void(MsgType type,
                                            std::vector<uint8_t> payload,
                                            const std::string& excludePeerId)>;
    using LogFn       = std::function<void(int level,
                                            const std::string& msg)>;
    using BlockFn     = std::function<void(const Block&,
                                            const std::string& peerId)>;
    using TxFn        = std::function<void(const Transaction&,
                                            const std::string& peerId)>;

    explicit MessageHandler(Config                       cfg,
                             Blockchain&                  chain,
                             Mempool&                     mempool,
                             std::shared_ptr<PeerManager> peerMgr);

    MessageHandler(const MessageHandler&)            = delete;
    MessageHandler& operator=(const MessageHandler&) = delete;

    // Install callbacks — safe to call at any time
    void setSendFn      (SendFn fn)      noexcept;
    void setBroadcastFn (BroadcastFn fn) noexcept;
    void setLogFn       (LogFn fn)       noexcept;
    void onBlock        (BlockFn fn)     noexcept;
    void onTransaction  (TxFn fn)        noexcept;

    // Entry point — called for every inbound raw frame from a peer.
    // Thread-safe: may be called concurrently from multiple IO threads.
    // All user callbacks are wrapped in try/catch inside the implementation.
    void handleRaw(const std::string&          peerId,
                   const std::vector<uint8_t>& raw);

    // Outbound builders — all return encoded wire frames ready to send.
    // All functions are const and thread-safe.
    std::vector<uint8_t> buildHandshake  ()                              const;
    std::vector<uint8_t> buildPing       ()                              const;
    std::vector<uint8_t> buildPong       ()                              const;
    std::vector<uint8_t> buildGetPeers   ()                              const;
    std::vector<uint8_t> buildBlock      (const Block& b)                const;
    std::vector<uint8_t> buildTx         (const Transaction& tx)         const;
    std::vector<uint8_t> buildTxBatch    (const std::vector<Transaction>&) const;
    std::vector<uint8_t> buildGetHeaders (uint64_t fromHeight)           const;
    std::vector<uint8_t> buildReject     (const std::string& reason)     const;

    // Frame encode — returns empty vector on failure (oversized or alloc)
    static std::vector<uint8_t> encodeFrame(
        MsgType                     type,
        const std::vector<uint8_t>& payload,
        const std::string&          magic) noexcept;

    // Frame decode — structured error result; validates all fields
    // before any heap allocation. Payload length validated against
    // MAX_PAYLOAD_SIZE before allocation (DoS prevention).
    static FrameDecodeResult decodeFrame(
        const std::vector<uint8_t>& raw,
        const std::string&          magic,
        WireFrame&                  out) noexcept;

private:
    Config                       cfg_;
    Blockchain&                  chain_;
    Mempool&                     mempool_;
    std::shared_ptr<PeerManager> peerMgr_;

    // Per-type mutexes — prevent cross-callback blocking
    mutable std::mutex sendMu_;
    mutable std::mutex broadcastMu_;
    mutable std::mutex logMu_;
    mutable std::mutex blockMu_;
    mutable std::mutex txMu_;

    SendFn      sendFn_;
    BroadcastFn broadcastFn_;
    LogFn       logFn_;
    BlockFn     blockFn_;
    TxFn        txFn_;

    // Structured logger — wraps logFn_ in try/catch
    void slog(int level, const std::string& msg) const noexcept;

    // Safe callback helpers — wrap all user callbacks in try/catch.
    // Log errors via slog; never propagate exceptions into the network loop.
    void invokeSend     (const std::string& peerId,
                          std::vector<uint8_t> frame)      const noexcept;
    void invokeBroadcast(MsgType type,
                          std::vector<uint8_t> payload,
                          const std::string& excludeId)    const noexcept;
    void invokeBlock    (const Block& b,
                          const std::string& peerId)        const noexcept;
    void invokeTx       (const Transaction& tx,
                          const std::string& peerId)        const noexcept;

    // Dispatch — one method per message type
    void handleHandshake   (const std::string& peerId,
                             const std::vector<uint8_t>& payload);
    void handleHandshakeAck(const std::string& peerId,
                             const std::vector<uint8_t>& payload);
    void handlePing        (const std::string& peerId);
    void handlePong        (const std::string& peerId);
    void handleGetPeers    (const std::string& peerId);
    void handlePeers       (const std::string& peerId,
                             const std::vector<uint8_t>& payload);
    void handleBlock       (const std::string& peerId,
                             const std::vector<uint8_t>& payload);
    void handleTransaction (const std::string& peerId,
                             const std::vector<uint8_t>& payload);
    void handleTxBatch     (const std::string& peerId,
                             const std::vector<uint8_t>& payload);
    void handleGetHeaders  (const std::string& peerId,
                             const std::vector<uint8_t>& payload);
    void handleHeaders     (const std::string& peerId,
                             const std::vector<uint8_t>& payload);
    void handleReject      (const std::string& peerId,
                             const std::vector<uint8_t>& payload);

    // Wire serialization helpers — all bounds-checked
    static void        writeUint32BE(std::vector<uint8_t>& buf, uint32_t v) noexcept;
    static void        writeUint64BE(std::vector<uint8_t>& buf, uint64_t v) noexcept;
    static uint32_t    readUint32BE (const uint8_t* p)                      noexcept;
    static uint64_t    readUint64BE (const uint8_t* p)                      noexcept;
    static void        writeString  (std::vector<uint8_t>& buf,
                                      const std::string& s)                 noexcept;
    // Returns empty string and advances offset=max on bounds violation
    static std::string readString   (const uint8_t* p,
                                      size_t& offset,
                                      size_t max)                           noexcept;
};

} // namespace net
