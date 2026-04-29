#include "net/p2p_node.h"
#include "net/message_handler.h"
#include "blockchain/consensus.h"
#include "serialization/codec.h"
#include "crypto/sha256.h"

#include <boost/asio/ssl.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/endian/conversion.hpp>

namespace net {

// === METRICS, BUFFER POOL, & RECONNECT (Your original logic, hardened) ===
static P2PMetrics g_metrics;
static BufferPool g_bufferPool;
static ReconnectTracker g_reconnect;

class Session : public std::enable_shared_from_this<Session> {
public:
    static constexpr uint32_t MAGIC_MEDOR = 0x4D454452; // "MEDR"
    static constexpr size_t HEADER_SIZE = 12;

    Session(boost::asio::io_context& ioc, boost::asio::ssl::context& sslCtx, 
            std::shared_ptr<MessageHandler> handler, const P2PNode::Config& cfg)
        : socket_(ioc, sslCtx), msgHandler_(std::move(handler)), cfg_(cfg),
          rateLimiter_(100, 10) { // 100 msgs/sec, burst of 10
        g_metrics.sessionsCreated.fetch_add(1, std::memory_order_relaxed);
    }

    void start() {
        auto self = shared_from_this();
        socket_.async_handshake(boost::asio::ssl::stream_base::client,
            [this, self](const boost::system::error_code& ec) {
                if (ec) return disconnect("TLS Handshake Failed: " + ec.message());
                sendVersion(); // Step 1: Send 8888 Handshake
                doReadHeader();
            });
    }

    // 1. ASYNC SEND QUEUE (Handles Backpressure)
    void send(std::shared_ptr<std::vector<uint8_t>> data) {
        auto self = shared_from_this();
        boost::asio::post(socket_.get_executor(), [this, self, data]() {
            bool idle = sendQueue_.empty();
            sendQueue_.push_back(data);
            if (idle) doWrite();
        });
    }

private:
    // 2. PRODUCTION HANDSHAKE (Building the 8888 Payload)
    void sendVersion() {
        std::vector<uint8_t> payload;
        appendU32(payload, 8888); // Chain ID
        appendU64(payload, uint64_t(std::time(nullptr)));
        appendU64(payload, msgHandler_->getCurrentHeight());
        
        send(wrapFrame(0x01, payload)); // 0x01 = MSG_VERSION
    }

    // 3. FULL INTEGRITY: Double-SHA256 Checksum Verification
    void doReadHeader() {
        auto self = shared_from_this();
        boost::asio::async_read(socket_, boost::asio::buffer(headerIn_, HEADER_SIZE),
            [this, self](const boost::system::error_code& ec, size_t) {
                if (ec) return disconnect("Header read failed");

                uint32_t magic = readU32(headerIn_, 0);
                uint32_t len = readU32(headerIn_, 4);
                uint32_t checksum = readU32(headerIn_, 8);

                if (magic != MAGIC_MEDOR || len > cfg_.maxPayloadSize) {
                    g_metrics.oversizedFrames.fetch_add(1);
                    return disconnect("Protocol Violation: Invalid Header");
                }
                doReadPayload(len, checksum);
            });
    }

    void doReadPayload(uint32_t len, uint32_t expectedChecksum) {
        auto self = shared_from_this();
        auto buf = std::make_shared<std::vector<uint8_t>>(len);
        boost::asio::async_read(socket_, boost::asio::buffer(buf->data(), len),
            [this, self, buf, expectedChecksum](const boost::system::error_code& ec, size_t) {
                if (ec) return disconnect("Payload read failed");

                // REAL CHECKSUM VERIFICATION
                if (crypto::DoubleSHA256(buf->data(), buf->size()).truncated32() != expectedChecksum) {
                    return disconnect("Integrity Failure: Checksum Mismatch");
                }

                // 4. RATE LIMITING & SPAM PROTECTION
                if (!rateLimiter_.consume(1)) {
                    g_metrics.peersBanned.fetch_add(1);
                    return disconnect("Rate limit exceeded");
                }

                handleMessage(buf);
            });
    }

    void doWrite() {
        auto self = shared_from_this();
        boost::asio::async_write(socket_, boost::asio::buffer(*sendQueue_.front()),
            [this, self](const boost::system::error_code& ec, size_t bytes) {
                if (ec) return disconnect("Write error");
                g_metrics.bytesSent.fetch_add(bytes);
                sendQueue_.pop_front();
                if (!sendQueue_.empty()) doWrite();
            });
    }

    // 5. BLOCK/TX DISPATCH WITH ERROR CLEANUP
    void handleMessage(std::shared_ptr<std::vector<uint8_t>> data) {
        try {
            auto msg = Codec::deserialize(*data);
            if (msg.type == MsgType::BLOCK) {
                if (Consensus::verifyBlock(msg.block)) {
                    msgHandler_->onBlock(msg.block);
                    g_metrics.broadcastsSent.fetch_add(1);
                }
            } else if (msg.type == MsgType::TX) {
                if (Consensus::verifyTransaction(msg.tx)) {
                    msgHandler_->onTransaction(msg.tx);
                }
            }
        } catch (...) {
            g_metrics.recvErrors.fetch_add(1);
            disconnect("Malformed payload");
        }
    }

    std::shared_ptr<std::vector<uint8_t>> wrapFrame(uint8_t type, const std::vector<uint8_t>& payload) {
        auto frame = g_bufferPool.acquire();
        appendU32(*frame, MAGIC_MEDOR);
        appendU32(*frame, uint32_t(payload.size() + 1));
        
        std::vector<uint8_t> fullData = {type};
        fullData.insert(fullData.end(), payload.begin(), payload.end());
        
        appendU32(*frame, crypto::DoubleSHA256(fullData.data(), fullData.size()).truncated32());
        frame->insert(frame->end(), fullData.begin(), fullData.end());
        return frame;
    }
};

// =============================================================================
// P2PNODE (High-Level Registry & Metrics)
// =============================================================================
void P2PNode::broadcastBlock(const Block& b) {
    auto frame = wrapBlockInHeader(b); 
    std::shared_lock lk(peersMu_);
    for (auto& [id, session] : peers_) {
        if (session->isAuthenticated()) session->send(frame);
    }
}

} // namespace net
