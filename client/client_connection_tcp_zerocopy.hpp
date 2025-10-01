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
#include "network_transport.hpp"
#include "common/mmap_buffer_pool.hpp"
#include "common/buffer_pool.hpp"
#include "common/message/message.hpp"

// system headers
#include <netinet/tcp.h>

// 3rd party headers
#include <boost/asio/steady_timer.hpp>

// standard headers
#include <atomic>
#include <memory>
#include <mutex>
#include <chrono>
#include <queue>

/// TRUE Zero-copy TCP client connection using TCP_ZEROCOPY_RECEIVE
/**
 * This connection provides TRUE zero-copy receive using mmap and TCP_ZEROCOPY_RECEIVE.
 *
 * Key features:
 * - Uses TCP_ZEROCOPY_RECEIVE getsockopt for direct kernel data mapping
 * - MmapBufferPool provides page-aligned buffers required for zero-copy
 * - Falls back to regular recv() when zero-copy conditions not met
 * - No MSG_ZEROCOPY or MSG_ERRQUEUE (those are for send-side only)
 * - Comprehensive statistics for zero-copy success/failure analysis
 *
 * Zero-copy requirements:
 * - Buffer must be page-aligned (handled by MmapBufferPool)
 * - Buffer size must be multiple of page size
 * - Incoming data must align on page boundaries for optimal performance
 * - loopback doesn't support TCP zero-copy due to missing header-data split
 */
class ClientConnectionTcpZeroCopy : public ClientConnection, public client::NetworkTransport
{
public:
    /// c'tor
    ClientConnectionTcpZeroCopy(boost::asio::io_context& io_context, ClientSettings::Server server);
    /// d'tor
    ~ClientConnectionTcpZeroCopy() override;

    // ClientConnection interface

    void disconnect() override;
    std::string getMacAddress() override;
    void getNextMessage(const MessageHandler<msg::BaseMessage>& handler) override;

    // NetworkTransport interface
    void connect(ConnectCallback callback) override;
    void send(std::shared_ptr<msg::BaseMessage> message, SendCallback callback) override;
    void receiveMessage(MessageCallback callback) override;

protected:
    /// non-virtual version of disconnect(), for d'tor
    void close();

    /// non-virtual version of connect(), for d'tor
    boost::system::error_code doConnect(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint) override;

    /// async write using boost::asio
    void write(boost::asio::streambuf& buffer, WriteHandler&& write_handler) override;

    /// Zero-copy receive statistics
    struct ZeroCopyStats
    {
        std::atomic<uint64_t> zerocopy_attempts{0};      ///< TCP_ZEROCOPY_RECEIVE attempts
        std::atomic<uint64_t> zerocopy_successful{0};    ///< Successful zero-copy receives
        std::atomic<uint64_t> zerocopy_bytes{0};         ///< Bytes received via zero-copy
        std::atomic<uint64_t> regular_receives{0};       ///< Fallback to regular recv()
        std::atomic<uint64_t> regular_bytes{0};          ///< Bytes via regular recv()
        std::atomic<uint64_t> fallback_page_misalign{0}; ///< Fallbacks due to page misalignment
        std::atomic<uint64_t> fallback_size_mismatch{0}; ///< Fallbacks due to size issues

        /// Calculate zero-copy success rate as percentage
        double getSuccessRate() const {
            uint64_t attempts = zerocopy_attempts.load();
            return attempts > 0 ? (100.0 * zerocopy_successful.load() / attempts) : 0.0;
        }

    };

    /// Get current zero-copy statistics
    const ZeroCopyStats& getStats() const { return stats_; }

    /// Log zero-copy statistics (called periodically)
    void logZeroCopyStats() const;

private:
    /// Wait for data availability and try zero-copy receive
    void waitForDataAndTryZeroCopy(size_t expected_size, const MessageHandler<msg::BaseMessage>& handler);

    /// Try to receive message using TCP_ZEROCOPY_RECEIVE
    bool tryZeroCopyReceive(size_t expected_size, const MessageHandler<msg::BaseMessage>& handler);

    /// Fallback to regular async receive
    void receiveRegular(size_t message_size, const MessageHandler<msg::BaseMessage>& handler);

    /// Check if size is suitable for zero-copy (must be multiple of page size)
    bool isSuitableForZeroCopy(size_t size) const;

    /// Get native socket handle for getsockopt
    int getNativeSocket() const;

    /// Process zero-copy received data
    void processZeroCopyData(void* mapped_data, size_t data_size, const MessageHandler<msg::BaseMessage>& handler);

    /// Initialize periodic statistics logging
    void initStatsLogging();

    /// Periodic statistics logging callback
    void onStatsTimer(const boost::system::error_code& error);

private:
    /// Page-aligned mmap buffer pool for zero-copy (initialized on first use)
    std::unique_ptr<MmapBufferPool> mmap_buffer_pool_;

    /// Zero-copy statistics
    mutable ZeroCopyStats stats_;

    /// Statistics logging timer (every 30 seconds)
    boost::asio::steady_timer stats_timer_;

    /// Page size for alignment calculations
    static const size_t PAGE_SIZE;

    /// Minimum message size to attempt zero-copy (kernel typically requires 4KB+ for effective zero-copy)
    static constexpr size_t MIN_ZEROCOPY_SIZE = 4096;

    /// Thread safety for statistics access
    mutable std::mutex stats_mutex_;
    
    /// TCP socket
    boost::asio::ip::tcp::socket socket_;
    
    /// Regular buffer pool for boost::asio operations
    DynamicBufferPool& buffer_pool_;


    // ============ Controlled Async Loop Infrastructure ============

    /// Maximum number of concurrent read operations (3: audio, control, timestamps)
    static constexpr size_t MAX_CONCURRENT_READS = 3;

    /// Number of currently active read operations
    std::atomic<size_t> active_reads_{0};

    /// Queue for incoming message handlers waiting for processing
    std::queue<MessageHandler<msg::BaseMessage>> pending_handlers_;

    /// Mutex to protect pending handlers queue
    std::mutex handlers_mutex_;

    /// Start a single controlled read operation
    void startControlledRead();

    /// Handle completion of a controlled read and potentially start next one
    void onControlledReadComplete(const boost::system::error_code& ec, std::unique_ptr<msg::BaseMessage> response);

    /// Hide messageReceived to ensure handler is always called for controlled reading
    void messageReceived(std::unique_ptr<msg::BaseMessage> message, const MessageHandler<msg::BaseMessage>& handler);

public:
};
