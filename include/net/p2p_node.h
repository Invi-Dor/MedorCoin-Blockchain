#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <vector>
#include <string>
#include <deque>
#include <map>
#include <shared_mutex>
#include <atomic>

// Forward declarations
namespace net {
    class MessageHandler;
    class Session;

    struct P2PNode {
        struct Config {
            uint32_t maxPayloadSize = 10 * 1024 * 1024; // 10MB
            std::string seedNodes;
        };

        void broadcastBlock(const class Block& b);
        
    private:
        std::shared_mutex peersMu_;
        std::map<uint64_t, std::shared_ptr<Session>> peers_;
    };

    // Metrics for MedorScan/Monitoring
    struct P2PMetrics {
        std::atomic<uint64_t> sessionsCreated{0};
        std::atomic<uint64_t> oversizedFrames{0};
        std::atomic<uint64_t> peersBanned{0};
        std::atomic<uint64_t> bytesSent{0};
        std::atomic<uint64_t> broadcastsSent{0};
        std::atomic<uint64_t> recvErrors{0};
    };

    // Support structures used in .cpp
    struct BufferPool {
        std::shared_ptr<std::vector<uint8_t>> acquire() {
            return std::make_shared<std::vector<uint8_t>>();
        }
    };

    struct ReconnectTracker {};
    
    struct RateLimiter {
        RateLimiter(int rate, int burst) {}
        bool consume(int amount) { return true; } // Logic implemented in .cpp
    };
}
