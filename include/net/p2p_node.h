#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations to keep the header lightweight
class MessageHandler;
class Block;

namespace net {

/**
 * @class P2PNode
 * @brief High-performance network host for the MedorCoin blockchain.
 * Manages peer connections and public listening on the specified port.
 */
class P2PNode : public std::enable_shared_from_this<P2PNode> {
public:
    /**
     * @struct Config
     * @brief Essential network parameters.
     */
    struct Config {
        uint16_t port = 30303;
        uint32_t chainId = 8888;
        size_t maxPayloadSize = 8 * 1024 * 1024; // 8MB DoS Protection
    };

    /**
     * @brief Initializes the node but does not start listening yet.
     */
    P2PNode(boost::asio::io_context& ioc, 
            boost::asio::ssl::context& sslCtx, 
            const Config& cfg);

    /**
     * @brief Starts the async accept loop to receive public connections.
     */
    void start();

    /**
     * @brief Propagates a new block to all authenticated peers.
     */
    void broadcastBlock(const Block& b);

private:
    void doAccept();

    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& sslCtx_;
    Config cfg_;
    
    boost::asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<MessageHandler> msgHandler_;

    // Thread-safe peer management
    mutable std::shared_mutex peersMu_;
    std::unordered_map<std::string, std::shared_ptr<class Session>> peers_;
};

/**
 * @struct P2PMetrics
 * @brief Thread-safe counters for monitoring node health.
 */
struct P2PMetrics {
    std::atomic<uint64_t> sessionsCreated{0};
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<uint64_t> oversizedFrames{0};
    std::atomic<uint64_t> peersBanned{0};
    std::atomic<uint64_t> recvErrors{0};
    std::atomic<uint64_t> broadcastsSent{0};
};

} // namespace net
