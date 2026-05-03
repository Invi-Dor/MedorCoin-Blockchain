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

// === METRICS & SHARED STATE ===
static P2PMetrics g_metrics;
static BufferPool g_bufferPool;
static ReconnectTracker g_reconnect;

/**
 * @class Session
 * @brief Handles the 1-on-1 encrypted link with another peer.
 */
class Session : public std::enable_shared_from_this<Session> {
public:
    static constexpr uint32_t MAGIC_MEDOR = 0x4D454452; // "MEDR"
    static constexpr size_t HEADER_SIZE = 12;

    Session(boost::asio::io_context& ioc, boost::asio::ssl::context& sslCtx, 
            std::shared_ptr<MessageHandler> handler, const P2PNode::Config& cfg)
        : socket_(ioc, sslCtx), msgHandler_(std::move(handler)), cfg_(cfg),
          rateLimiter_(100, 10) {
        g_metrics.sessionsCreated.fetch_add(1, std::memory_order_relaxed);
    }

    void start() {
        auto self = shared_from_this();
        socket_.async_handshake(boost::asio::ssl::stream_base::server,
            [this, self](const boost::system::error_code& ec) {
                if (ec) return disconnect("TLS Failed");
                sendVersion();
                doReadHeader();
            });
    }

    void send(std::shared_ptr<std::vector<uint8_t>> data) {
        auto self = shared_from_this();
        boost::asio::post(socket_.get_executor(), [this, self, data]() {
            bool idle = sendQueue_.empty();
            sendQueue_.push_back(data);
            if (idle) doWrite();
        });
    }

    bool isAuthenticated() const { return authenticated_; }

private:
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> socket_;
    std::shared_ptr<MessageHandler> msgHandler_;
    P2PNode::Config cfg_;
    uint8_t headerIn_[HEADER_SIZE];
    std::deque<std::shared_ptr<std::vector<uint8_t>>> sendQueue_;
    crypto::TokenBucket rateLimiter_;
    bool authenticated_ = false;

    void sendVersion() {
        std::vector<uint8_t> payload;
        appendU32(payload, 8888); // Chain ID
        appendU64(payload, uint64_t(std::time(nullptr)));
        appendU64(payload, msgHandler_->getCurrentHeight());
        send(wrapFrame(0x01, payload));
    }

    void doReadHeader() {
        auto self = shared_from_this();
        boost::asio::async_read(socket_, boost::asio::buffer(headerIn_, HEADER_SIZE),
            [this, self](const boost::system::error_code& ec, size_t) {
                if (ec) return disconnect("Read error");
                uint32_t magic = readU32(headerIn_, 0);
                uint32_t len = readU32(headerIn_, 4);
                uint32_t checksum = readU32(headerIn_, 8);
                if (magic != MAGIC_MEDOR || len > cfg_.maxPayloadSize) return disconnect("Magic fail");
                doReadPayload(len, checksum);
            });
    }

    void doReadPayload(uint32_t len, uint32_t expectedChecksum) {
        auto self = shared_from_this();
        auto buf = std::make_shared<std::vector<uint8_t>>(len);
        boost::asio::async_read(socket_, boost::asio::buffer(buf->data(), len),
            [this, self, buf, expectedChecksum](const boost::system::error_code& ec, size_t) {
                if (ec || crypto::DoubleSHA256(buf->data(), buf->size()).truncated32() != expectedChecksum) 
                    return disconnect("Payload fail");
                handleMessage(buf);
            });
    }

    void handleMessage(std::shared_ptr<std::vector<uint8_t>> data) {
        try {
            auto msg = Codec::deserialize(*data);
            if (msg.type == MsgType::VERSION && msg.chainId == 8888) {
                authenticated_ = true;
            } else if (authenticated_) {
                if (msg.type == MsgType::BLOCK) msgHandler_->onBlock(msg.block);
                if (msg.type == MsgType::TX) msgHandler_->onTransaction(msg.tx);
            }
        } catch (...) { disconnect("Codec fail"); }
    }

    void doWrite() {
        auto self = shared_from_this();
        boost::asio::async_write(socket_, boost::asio::buffer(*sendQueue_.front()),
            [this, self](const boost::system::error_code& ec, size_t) {
                if (ec) return disconnect("Write fail");
                sendQueue_.pop_front();
                if (!sendQueue_.empty()) doWrite();
            });
    }

    void disconnect(const std::string& r) { socket_.lowest_layer().close(); }

    std::shared_ptr<std::vector<uint8_t>> wrapFrame(uint8_t type, const std::vector<uint8_t>& p) {
        auto f = g_bufferPool.acquire();
        appendU32(*f, MAGIC_MEDOR);
        appendU32(*f, uint32_t(p.size() + 1));
        std::vector<uint8_t> body = {type};
        body.insert(body.end(), p.begin(), p.end());
        appendU32(*f, crypto::DoubleSHA256(body.data(), body.size()).truncated32());
        f->insert(f->end(), body.begin(), body.end());
        return f;
    }
};

// =============================================================================
// P2PNODE (The Network Host)
// =============================================================================

P2PNode::P2PNode(boost::asio::io_context& ioc, boost::asio::ssl::context& sslCtx, const Config& cfg)
    : ioc_(ioc), sslCtx_(sslCtx), cfg_(cfg), acceptor_(ioc) {
    
    // BINDS TO PUBLIC IP (68.218.39.194)
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), cfg.port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();

    doAccept();
}

void P2PNode::doAccept() {
    acceptor_.async_accept([this](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
        if (!ec) {
            auto session = std::make_shared<Session>(ioc_, sslCtx_, msgHandler_, cfg_);
            session->start();
            std::unique_lock lk(peersMu_);
            peers_[socket.remote_endpoint().address().to_string()] = session;
        }
        doAccept();
    });
}

void P2PNode::broadcastBlock(const Block& b) {
    // Logic to wrap block and send to all authenticated peers
}

} // namespace net
