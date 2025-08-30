/***
    This file is part of snapcast
    Copyright (C) 2014-2025  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#pragma once

// local headers
#include "stream_session_tcp.hpp"

// 3rd party headers
#include <boost/asio/steady_timer.hpp>

// standard headers
#include <atomic>
#include <memory>
#include <vector>
#include <deque>
#include <sys/socket.h>
#include <linux/net_tstamp.h>

using boost::asio::ip::tcp;

/// Hybrid TCP session with selective zerocopy for large messages
/**
 * This session extends the regular TCP session with selective zerocopy capability.
 * - Small messages (headers, control data) use regular Boost.Asio async_write
 * - Large messages (PCM audio chunks >1KB) can use MSG_ZEROCOPY selectively
 * - Maintains full compatibility with existing protocol and client expectations
 * - Provides graceful fallback when zerocopy is not available
 * - Uses separate error queue monitoring outside Boost.Asio async flow
 */
class StreamSessionTcpHybrid : public StreamSessionTcp
{
public:
    StreamSessionTcpHybrid(StreamMessageReceiver* receiver, const ServerSettings& server_settings, tcp::socket&& socket);
    ~StreamSessionTcpHybrid() override;

    void start() override;
    void stop() override;

    /// Get zerocopy statistics for this session
    struct ZeroCopyStats
    {
        uint64_t zerocopy_attempts{0};      // Total zerocopy send attempts
        uint64_t zerocopy_successful{0};    // Successful zerocopy sends
        uint64_t zerocopy_bytes{0};         // Total bytes sent via zerocopy
        uint64_t regular_sends{0};          // Fallback to regular sends
        uint64_t regular_bytes{0};          // Total bytes sent via regular TCP
        uint64_t completions_received{0};   // Error queue completions received
        uint64_t pending_buffers{0};        // Currently pending zerocopy buffers
        double zerocopy_percentage() const 
        { 
            return (zerocopy_attempts + regular_sends) > 0 ? 
                   (double(zerocopy_successful) / double(zerocopy_attempts + regular_sends)) * 100.0 : 0.0; 
        }
    };
    
    ZeroCopyStats getZeroCopyStats() const;

protected:
    void sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler) override;

private:
    /// Initialize zerocopy capability
    bool initializeZeroCopy();
    
    /// Send using zerocopy (for large messages)
    void sendZeroCopy(const shared_const_buffer& buffer, WriteHandler&& handler);
    
    /// Send using regular TCP (for small messages or fallback)
    void sendRegular(const shared_const_buffer& buffer, WriteHandler&& handler);
    
    /// Monitor error queue for zerocopy completions
    void startErrorQueueMonitoring();
    void stopErrorQueueMonitoring();
    void processErrorQueue();
    
    /// Buffer management for zerocopy
    struct ZeroCopyBuffer
    {
        std::shared_ptr<std::vector<char>> data;
        WriteHandler handler;
        uint32_t id;
        size_t size;
    };
    
    // Configuration
    static constexpr size_t ZEROCOPY_THRESHOLD = 1024;  // Use zerocopy for messages >1KB
    static constexpr size_t MAX_PENDING_BUFFERS = 64;   // Limit pending zerocopy buffers
    
    // Zerocopy state
    bool zerocopy_available_{false};
    int native_socket_{-1};
    std::atomic<uint32_t> next_buffer_id_{1};
    
    // Statistics (thread-safe)
    mutable std::atomic<uint64_t> zerocopy_attempts_{0};
    mutable std::atomic<uint64_t> zerocopy_successful_{0};
    mutable std::atomic<uint64_t> zerocopy_bytes_{0};
    mutable std::atomic<uint64_t> regular_sends_{0};
    mutable std::atomic<uint64_t> regular_bytes_{0};
    mutable std::atomic<uint64_t> completions_received_{0};
    
    // Buffer tracking
    std::deque<ZeroCopyBuffer> pending_buffers_;
    mutable std::mutex pending_buffers_mutex_;
    
    // Error queue monitoring
    std::unique_ptr<boost::asio::steady_timer> error_queue_timer_;
    bool monitoring_active_{false};
};