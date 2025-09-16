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
#include "client_connection.hpp"
#include "common/buffer_pool.hpp"

// 3rd party headers
#include <boost/asio/steady_timer.hpp>

// standard headers
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>

/// Zero-copy TCP client connection for receiving audio chunks
/**
 * This connection extends the regular TCP connection with zero-copy receive capability.
 * - Uses MSG_ZEROCOPY with recvmsg() for large audio chunks (>1KB)
 * - Falls back to regular async_read for small control messages
 * - Provides comprehensive statistics and periodic logging
 */
class ClientConnectionTcpZeroCopy : public ClientConnectionTcp
{
public:
    ClientConnectionTcpZeroCopy(boost::asio::io_context& io_context, ClientSettings::Server server);
    ~ClientConnectionTcpZeroCopy() override;

    void disconnect() override;
    void getNextMessage(const MessageHandler<msg::BaseMessage>& handler) override;

    /// SERVER COMPLIANCE: Get zero-copy statistics matching server format
    struct ZeroCopyStats
    {
        uint64_t zerocopy_attempts{0};           // Total zero-copy receive attempts
        uint64_t zerocopy_successful{0};         // Successful zero-copy receives
        uint64_t zerocopy_bytes{0};              // Total bytes received via zero-copy
        uint64_t regular_receives{0};            // Messages received via regular async_read
        uint64_t regular_bytes{0};               // Total bytes received via regular async_read
        uint64_t coordination_fallbacks{0};     // SERVER COMPLIANCE: Fallbacks due to pending async ops
        uint64_t pending_async_operations{0};   // SERVER COMPLIANCE: Currently pending async operations
        uint64_t outstanding_zerocopy_buffers{0}; // SERVER COMPLIANCE: Outstanding zerocopy operations
        uint64_t completion_notifications_received{0}; // SERVER COMPLIANCE: Completion notifications received
        uint64_t completion_notifications_missing{0};  // SERVER COMPLIANCE: Missing notifications
        uint64_t buffers_completed_via_notifications{0}; // SERVER COMPLIANCE: Completed via notifications
        
        double zerocopy_percentage() const 
        { 
            return (zerocopy_attempts + regular_receives) > 0 ? 
                   (double(zerocopy_successful) / double(zerocopy_attempts + regular_receives)) * 100.0 : 0.0; 
        }
        
        double completion_reliability() const
        {
            return zerocopy_successful > 0 ?
                   (double(buffers_completed_via_notifications) / double(zerocopy_successful)) * 100.0 : 0.0;
        }
    };
    
    ZeroCopyStats getZeroCopyStats() const;
    void resetZeroCopyStats();

private:
    /// Initialize zero-copy capability
    bool initializeZeroCopy();
    
    /// SERVER COMPLIANCE: Try to reserve zero-copy access (atomic coordination)
    bool tryReserveZeroCopy();
    
    /// SERVER COMPLIANCE: Release zero-copy reservation
    void releaseZeroCopy();
    
    /// SERVER COMPLIANCE: Check if we can safely use zero-copy (no pending async operations)
    bool canUseZeroCopy() const;
    
    /// Try to receive using coordinated zero-copy for large messages
    bool tryZeroCopyReceive(size_t message_size, const MessageHandler<msg::BaseMessage>& handler);
    
    /// Receive using coordinated zero-copy
    void receiveZeroCopyCoordinated(size_t message_size, const MessageHandler<msg::BaseMessage>& handler);
    
    /// Receive using regular async_read with coordination tracking
    void receiveRegularCoordinated(size_t message_size, const MessageHandler<msg::BaseMessage>& handler);
    
    /// Start periodic statistics logging
    void startPeriodicLogging();
    void stopPeriodicLogging();
    void scheduleNextStatisticsLog();
    void logStatistics();
    
    // Configuration
    static constexpr size_t ZEROCOPY_THRESHOLD = 1024;  // Use zero-copy for messages >1KB
    static constexpr std::chrono::seconds STATS_LOG_INTERVAL{30}; // Log statistics every 30s
    
    // Zero-copy state
    bool zerocopy_available_{false};
    int native_socket_{-1};
    
    // SERVER COMPLIANCE: Coordination state
    std::atomic<uint32_t> pending_async_operations_{0};
    
    // SERVER COMPLIANCE: Statistics (thread-safe atomic counters)
    mutable std::atomic<uint64_t> zerocopy_attempts_{0};
    mutable std::atomic<uint64_t> zerocopy_successful_{0};
    mutable std::atomic<uint64_t> zerocopy_bytes_{0};
    mutable std::atomic<uint64_t> regular_receives_{0};
    mutable std::atomic<uint64_t> regular_bytes_{0};
    mutable std::atomic<uint64_t> coordination_fallbacks_{0};
    mutable std::atomic<uint64_t> outstanding_zerocopy_buffers_{0};
    mutable std::atomic<uint64_t> completion_notifications_received_{0};
    mutable std::atomic<uint64_t> completion_notifications_missing_{0};
    mutable std::atomic<uint64_t> buffers_completed_via_notifications_{0};
    
    // Periodic logging
    boost::asio::steady_timer stats_timer_;
    std::atomic<bool> logging_active_{false};
    
    // Buffer pool for efficient memory management
    DynamicBufferPool& buffer_pool_;
    
    // Zero-copy receive buffer management
    std::unique_ptr<char[]> zerocopy_buffer_;
    size_t zerocopy_buffer_size_{0};
};