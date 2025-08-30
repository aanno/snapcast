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
#include <thread>
#include <sys/socket.h>

using boost::asio::ip::tcp;

/// Dual-socket TCP session with separate zerocopy channel
/**
 * This session extends the regular TCP session with a separate zerocopy-enabled socket.
 * - Primary socket: Handled by Boost.Asio for all control messages and small data
 * - Zerocopy socket: Dedicated raw socket for large messages (PCM audio chunks)
 * - Complete separation prevents any interference between async and direct syscalls
 * - Maintains full protocol compatibility and provides graceful fallback
 */
class StreamSessionTcpDualSocket : public StreamSessionTcp
{
public:
    StreamSessionTcpDualSocket(StreamMessageReceiver* receiver, const ServerSettings& server_settings, tcp::socket&& socket);
    ~StreamSessionTcpDualSocket() override;

    void start() override;
    void stop() override;

    /// Get zerocopy statistics for this session
    struct ZeroCopyStats
    {
        uint64_t zerocopy_attempts{0};      // Total zerocopy send attempts  
        uint64_t zerocopy_successful{0};    // Successful zerocopy sends
        uint64_t zerocopy_bytes{0};         // Total bytes sent via zerocopy
        uint64_t regular_sends{0};          // Messages sent via primary socket
        uint64_t regular_bytes{0};          // Total bytes sent via primary socket
        uint64_t zerocopy_channel_active{0}; // Whether zerocopy channel is active
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
    /// Initialize zerocopy channel (separate socket connection)
    bool initializeZeroCopyChannel();
    
    /// Send using zerocopy channel (for large messages)
    void sendZeroCopy(const shared_const_buffer& buffer, WriteHandler&& handler);
    
    /// Send using primary socket (for small messages and control)
    void sendRegular(const shared_const_buffer& buffer, WriteHandler&& handler);
    
    /// Worker thread for zerocopy channel operations
    void zeroCopyWorker();
    void shutdownZeroCopyChannel();
    
    // Configuration
    static constexpr size_t ZEROCOPY_THRESHOLD = 1024;  // Use zerocopy for messages >1KB
    static constexpr int ZEROCOPY_PORT_OFFSET = 10;     // Zerocopy port = stream_port + offset
    
    // Zerocopy channel state
    bool zerocopy_channel_active_{false};
    int zerocopy_socket_{-1};
    std::string remote_address_;
    uint16_t zerocopy_port_{0};
    
    // Worker thread for zerocopy operations
    std::unique_ptr<std::thread> zerocopy_thread_;
    std::atomic<bool> shutdown_requested_{false};
    
    // Statistics (thread-safe)
    mutable std::atomic<uint64_t> zerocopy_attempts_{0};
    mutable std::atomic<uint64_t> zerocopy_successful_{0};
    mutable std::atomic<uint64_t> zerocopy_bytes_{0};
    mutable std::atomic<uint64_t> regular_sends_{0};
    mutable std::atomic<uint64_t> regular_bytes_{0};
};