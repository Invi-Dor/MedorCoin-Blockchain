#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <deque>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

// Forward declarations for integrated modules
class MessageHandler;
namespace crypto { class TokenBucket; }

namespace net {

using tcp = boost::asio::ip::tcp;
using ssl_sock = boost::asio::ssl::stream<tcp::socket>;

/**
 * @class P2PNode
 * @brief Blueprint for the MedorCoin Peer-to-Peer network node.
 */
class P2PNode : public std::enable_shared_from_this<P2PNode> {
public:
    struct Config {
        uint16_t port = 30303;
        uint32_t maxAttempts = 5;
        uint32_t chainId = 8888;
        size_t   maxPayloadSize = 8 * 1024 * 1024; // 8MB Hard Limit
    };

    explicit P2PNode(boost::asio::io_context& ioc, 
                     boost::asio::ssl::context& sslCtx, 
                     const Config& cfg);

    void start();
    void stop();

    // Handlers for blockchain data propagation
    void broadcastBlock(const class Block& block);
    void broadcastTransaction(const class Transaction& tx);

    // Metrics interface for observability
    struct P2PNodeMetrics getMetrics() const noexcept;

private:
    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& sslCtx_;
    Config cfg_;

    tcp::acceptor acceptor_;
    
    // Thread-safe peer management
    mutable std::shared_mutex peersMu_;
    std::unordered_map<std::string, std::shared_ptr<class Session>> peers_;

    void doAccept();
};

/**
 * @class Session
 * @brief Managed P2P connection lifecycle within the P2PNode.
 */
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::io_context& ioc, 
            boost::asio::ssl::context& sslCtx, 
            std::shared_ptr<MessageHandler> handler, 
            const P2PNode::Config& cfg);

    void start();
    void send(std::shared_ptr<std::vector<uint8_t>> data);
    bool isAuthenticated() const { return handshakeDone_; }

private:
    ssl_sock socket_;
    std::shared_ptr<MessageHandler> msgHandler_;
    P2PNode::Config cfg_;
    
    // Protocol state members
    bool handshakeDone_ = false;
    uint8_t headerIn_[12]; // Fixed size for [Magic][Length][Checksum]
    
    // Async backpressure & Rate limiting
    std::deque<std::shared_ptr<std::vector<uint8_t>>> sendQueue_;
    std::unique_ptr<crypto::TokenBucket> rateLimiter_;

    // Internal async orchestration
    void doReadHeader();
    void doReadPayload(uint32_t len, uint32_t expectedChecksum);
    void doWrite();
    void disconnect(const std::string& reason);
};

} // namespace net
