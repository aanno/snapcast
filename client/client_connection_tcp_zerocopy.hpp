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

/// TRUE Zero-copy TCP client connection using mmap-based receive
/**
 * This connection provides TRUE zero-copy receive using TCP_ZEROCOPY_RECEIVE.
 * - Uses mmap page-aligned buffers for direct kernel mapping
 * - Leverages TCP_ZEROCOPY_RECEIVE getsockopt for zero-copy
 * - Falls back to regular recv() when zero-copy conditions not met
 * - MmapBufferPool provides page-aligned RAII buffer management
 * - Comprehensive statistics for zero-copy success/failure analysis
 */
class ClientConnectionTcpZeroCopy : public ClientConnectionTcp
{
public:
    ClientConnectionTcpZeroCopy(boost::asio::io_context& io_context, ClientSettings::Server server);
    ~ClientConnectionTcpZeroCopy() override;

    void disconnect() override;
    void getNextMessage(const MessageHandler<msg::BaseMessage>& handler) override;

    /// Get zero-copy statistics for this connection
    struct ZeroCopyStats
    {
        uint64_t zerocopy_attempts{0};      // Total zero-copy receive attempts
        uint64_t zerocopy_successful{0};    // Successful zero-copy receives
        uint64_t zerocopy_bytes{0};         // Total bytes received via zero-copy
        uint64_t regular_receives{0};       // Messages received via regular async_read
        uint64_t regular_bytes{0};          // Total bytes received via regular async_read
        uint64_t large_message_fallbacks{0}; // Large messages that fell back to regular receive
        
        double zerocopy_percentage() const 
        { 
            return (zerocopy_attempts + regular_receives) > 0 ? 
                   (double(zerocopy_successful) / double(zerocopy_attempts + regular_receives)) * 100.0 : 0.0; 
        }
    };
    
    ZeroCopyStats getZeroCopyStats() const;
    void resetZeroCopyStats();

private:
    /// Initialize zero-copy capability
    bool initializeZeroCopy();
    
    /// Try to receive using TRUE zero-copy for large messages
    bool tryZeroCopyReceive(size_t message_size, const MessageHandler<msg::BaseMessage>& handler);
    
    /// Receive message body using regular async_read (fallback)
    void receiveRegular(size_t message_size, const MessageHandler<msg::BaseMessage>& handler);
    
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
    
    // Statistics (thread-safe)
    mutable std::atomic<uint64_t> zerocopy_attempts_{0};
    mutable std::atomic<uint64_t> zerocopy_successful_{0};
    mutable std::atomic<uint64_t> zerocopy_bytes_{0};
    mutable std::atomic<uint64_t> regular_receives_{0};
    mutable std::atomic<uint64_t> regular_bytes_{0};
    mutable std::atomic<uint64_t> large_message_fallbacks_{0};
    
    // Periodic logging
    boost::asio::steady_timer stats_timer_;
    std::atomic<bool> logging_active_{false};
    
    // Buffer pool for TRUE zero-copy memory management (no copies)
    DynamicBufferPool& buffer_pool_;
    
    // Header buffer for async_read (small fixed size)
    std::vector<char> header_buffer_;
};