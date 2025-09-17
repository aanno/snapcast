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
#include "common/mmap_buffer_pool.hpp"

// system headers
#include <netinet/tcp.h>

// 3rd party headers
#include <boost/asio/steady_timer.hpp>

// standard headers
#include <atomic>
#include <memory>
#include <mutex>
#include <chrono>

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
class ClientConnectionTcpZeroCopy : public ClientConnectionTcp
{
public:
    ClientConnectionTcpZeroCopy(boost::asio::io_context& io_context, ClientSettings::Server server);
    ~ClientConnectionTcpZeroCopy() override;

    void disconnect() override;
    void getNextMessage(const MessageHandler<msg::BaseMessage>& handler) override;

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
        std::atomic<uint64_t> mmap_buffer_hits{0};       ///< Buffer pool hits
        std::atomic<uint64_t> mmap_buffer_misses{0};     ///< Buffer pool misses

        /// Calculate zero-copy success rate as percentage
        double getSuccessRate() const {
            uint64_t attempts = zerocopy_attempts.load();
            return attempts > 0 ? (100.0 * zerocopy_successful.load() / attempts) : 0.0;
        }

        /// Calculate buffer pool hit rate as percentage
        double getBufferHitRate() const {
            uint64_t total = mmap_buffer_hits.load() + mmap_buffer_misses.load();
            return total > 0 ? (100.0 * mmap_buffer_hits.load() / total) : 0.0;
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
    /// Page-aligned mmap buffer pool for zero-copy
    MmapBufferPool mmap_buffer_pool_;

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
};
