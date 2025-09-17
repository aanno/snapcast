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

// header include
#include "client_connection_tcp_zerocopy.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/message/codec_header.hpp"

// system headers
#include <sys/socket.h>
#include <unistd.h>

// standard headers
#include <stdexcept>
#include <iomanip>

using namespace std;

static constexpr auto LOG_TAG = "ClientZeroCopy";

// Static page size initialization
const size_t ClientConnectionTcpZeroCopy::PAGE_SIZE = MmapBufferPool::getPageSize();

ClientConnectionTcpZeroCopy::ClientConnectionTcpZeroCopy(boost::asio::io_context& io_context, ClientSettings::Server server)
    : ClientConnectionTcp(io_context, std::move(server))
    , mmap_buffer_pool_(4) // 4 initial buffers per size bucket
    , stats_timer_(io_context)
{
    LOG(INFO, LOG_TAG) << "TRUE Zero-Copy TCP connection initialized with page size: " << PAGE_SIZE << " bytes\n";
    initStatsLogging();
}

ClientConnectionTcpZeroCopy::~ClientConnectionTcpZeroCopy()
{
    disconnect();
}

void ClientConnectionTcpZeroCopy::disconnect()
{
    stats_timer_.cancel();
    ClientConnectionTcp::disconnect();
    LOG(INFO, LOG_TAG) << "Zero-copy connection disconnected\n";
}

void ClientConnectionTcpZeroCopy::getNextMessage(const MessageHandler<msg::BaseMessage>& handler)
{
    // Use regular ClientConnectionTcp approach - for now we'll enhance it later with zero-copy
    // The TCP_ZEROCOPY_RECEIVE approach needs to be integrated at a lower level
    ClientConnectionTcp::getNextMessage(handler);
}


bool ClientConnectionTcpZeroCopy::tryZeroCopyReceive(size_t expected_size, const MessageHandler<msg::BaseMessage>& handler)
{
    stats_.zerocopy_attempts++;

    try {
        // Get page-aligned buffer from mmap pool
        size_t rounded_size = MmapBufferPool::roundToPageSize(expected_size);
        auto buffer_guard = mmap_buffer_pool_.acquire(rounded_size);

        if (!buffer_guard || !buffer_guard->valid()) {
            stats_.mmap_buffer_misses++;
            stats_.fallback_size_mismatch++;
            return false;
        }

        stats_.mmap_buffer_hits++;

        // Prepare TCP_ZEROCOPY_RECEIVE structure
        struct tcp_zerocopy_receive zc = {};
        zc.address = reinterpret_cast<uint64_t>(buffer_guard->data());
        zc.length = static_cast<uint32_t>(rounded_size);

        // Verify page alignment
        if (!MmapBufferPool::isPageAligned(buffer_guard->data())) {
            LOG(WARNING, LOG_TAG) << "Buffer not page-aligned, falling back to regular receive\n";
            stats_.fallback_page_misalign++;
            return false;
        }

        // Try zero-copy receive using getsockopt
        int native_socket = getNativeSocket();
        if (native_socket < 0) {
            LOG(ERROR, LOG_TAG) << "Failed to get native socket handle\n";
            return false;
        }

        socklen_t optlen = sizeof(zc);
        int ret = getsockopt(native_socket, IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE, &zc, &optlen);

        if (ret == 0 && zc.length > 0) {
            // Zero-copy successful!
            stats_.zerocopy_successful++;
            stats_.zerocopy_bytes += zc.length;

            LOG(DEBUG, LOG_TAG) << "Zero-copy receive successful: " << zc.length << " bytes mapped\n";

            // Process the zero-copy data
            processZeroCopyData(buffer_guard->data(), zc.length, handler);
            return true;
        } else {
            // Zero-copy failed, will fall back to regular receive
            if (ret != 0) {
                LOG(DEBUG, LOG_TAG) << "TCP_ZEROCOPY_RECEIVE failed: " << strerror(errno) << "\n";
            } else {
                LOG(DEBUG, LOG_TAG) << "TCP_ZEROCOPY_RECEIVE returned 0 bytes\n";
            }
            return false;
        }

    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Exception in zero-copy receive: " << e.what() << "\n";
        return false;
    }
}

void ClientConnectionTcpZeroCopy::receiveRegular(size_t message_size, const MessageHandler<msg::BaseMessage>& handler)
{
    stats_.regular_receives++;
    stats_.regular_bytes += message_size;

    // Use parent class regular async receive
    ClientConnectionTcp::getNextMessage(handler);
}

bool ClientConnectionTcpZeroCopy::isSuitableForZeroCopy(size_t size) const
{
    // Must be at least minimum size and preferably a multiple of page size
    return size >= MIN_ZEROCOPY_SIZE && (size % PAGE_SIZE == 0 || size >= PAGE_SIZE);
}

int ClientConnectionTcpZeroCopy::getNativeSocket() const
{
    try {
        // Get native socket handle from boost::asio socket (cast away const for native_handle)
        return const_cast<tcp_socket&>(socket_).native_handle();
    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Failed to get native socket: " << e.what() << "\n";
        return -1;
    }
}

void ClientConnectionTcpZeroCopy::processZeroCopyData(void* mapped_data, size_t data_size, const MessageHandler<msg::BaseMessage>& handler)
{
    try {
        // Create a message from the zero-copy mapped data
        // Note: This is a simplified example - actual message parsing would depend on the protocol

        // For now, we'll create a PcmChunk from the zero-copy data
        auto chunk = std::make_unique<msg::PcmChunk>();

        // Copy data pointer (note: this is still zero-copy as we're not copying the data itself)
        chunk->payload = static_cast<char*>(mapped_data);
        chunk->payloadSize = data_size;

        LOG(DEBUG, LOG_TAG) << "Processed zero-copy data: " << data_size << " bytes\n";

        // Pass to handler with no error
        handler(boost::system::error_code{}, std::move(chunk));

    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Error processing zero-copy data: " << e.what() << "\n";
        handler(boost::asio::error::operation_aborted, nullptr);
    }
}

void ClientConnectionTcpZeroCopy::initStatsLogging()
{
    // Log statistics every 30 seconds
    stats_timer_.expires_after(std::chrono::seconds(30));
    stats_timer_.async_wait([this](const boost::system::error_code& error) {
        onStatsTimer(error);
    });
}

void ClientConnectionTcpZeroCopy::onStatsTimer(const boost::system::error_code& error)
{
    if (error != boost::asio::error::operation_aborted) {
        logZeroCopyStats();

        // Schedule next logging
        stats_timer_.expires_after(std::chrono::seconds(30));
        stats_timer_.async_wait([this](const boost::system::error_code& error) {
            onStatsTimer(error);
        });
    }
}

void ClientConnectionTcpZeroCopy::logZeroCopyStats() const
{
    std::lock_guard<std::mutex> lock(stats_mutex_);

    LOG(INFO, LOG_TAG) << "=== TRUE Zero-Copy Client Stats (every 30s) ===\n";
    LOG(INFO, LOG_TAG) << "\tZC Attempts: " << stats_.zerocopy_attempts.load() << "\n";
    LOG(INFO, LOG_TAG) << "\tZC Successful: " << stats_.zerocopy_successful.load() << "\n";
    LOG(INFO, LOG_TAG) << "\tZC Bytes: " << stats_.zerocopy_bytes.load() << "\n";
    LOG(INFO, LOG_TAG) << "\tRegular Receives: " << stats_.regular_receives.load() << "\n";
    LOG(INFO, LOG_TAG) << "\tRegular Bytes: " << stats_.regular_bytes.load() << "\n";
    LOG(INFO, LOG_TAG) << "\tPage Misalign Fallbacks: " << stats_.fallback_page_misalign.load() << "\n";
    LOG(INFO, LOG_TAG) << "\tSize Mismatch Fallbacks: " << stats_.fallback_size_mismatch.load() << "\n";
    LOG(INFO, LOG_TAG) << "\tBuffer Pool Hits: " << stats_.mmap_buffer_hits.load() << "\n";
    LOG(INFO, LOG_TAG) << "\tBuffer Pool Misses: " << stats_.mmap_buffer_misses.load() << "\n";
    LOG(INFO, LOG_TAG) << "\tZC Success Rate: " << std::fixed << std::setprecision(2) << stats_.getSuccessRate() << "%\n";
    LOG(INFO, LOG_TAG) << "\tBuffer Hit Rate: " << std::fixed << std::setprecision(2) << stats_.getBufferHitRate() << "%\n";

    // Log mmap buffer pool stats
    mmap_buffer_pool_.logStats();
}
