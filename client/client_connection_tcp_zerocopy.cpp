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
#include "common/message/factory.hpp"

// 3rd party headers
#include <boost/asio/read.hpp>

// system headers
#include <sys/mman.h>
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
    // Step 1: Read message header normally (small, not worth zero-copy)
    auto header_buffer_guard = buffer_pool_.acquire(base_msg_size_);
    auto& header_buffer = header_buffer_guard.get();

    boost::asio::async_read(socket_, boost::asio::buffer(header_buffer.data(), base_msg_size_),
                           [this, handler, header_buffer_guard = std::move(header_buffer_guard)](boost::system::error_code ec, std::size_t length) mutable
    {
        if (ec)
        {
            LOG(ERROR, LOG_TAG) << "Error reading message header of length " << length << ": " << ec.message() << "\n";
            if (handler)
                handler(ec, nullptr);
            return;
        }

        // Parse header
        base_message_.deserialize(header_buffer_guard.get().data());
        tv t;
        base_message_.received = t;

        if (base_message_.type > message_type::kLast)
        {
            LOG(ERROR, LOG_TAG) << "unknown message type received: " << base_message_.type << ", size: " << base_message_.size << "\n";
            if (handler)
                handler(boost::asio::error::invalid_argument, nullptr);
            return;
        }
        else if (base_message_.size > msg::max_size)
        {
            LOG(ERROR, LOG_TAG) << "received message of type " << base_message_.type << " too large: " << base_message_.size << "\n";
            if (handler)
                handler(boost::asio::error::invalid_argument, nullptr);
            return;
        }

        // Step 2: For message body, try TCP_ZEROCOPY_RECEIVE if suitable
        if (isSuitableForZeroCopy(base_message_.size)) {
            LOG(DEBUG, LOG_TAG) << "Message size " << base_message_.size << " bytes suitable for zero-copy, waiting for data availability\n";
            // Wait for socket to have data ready before attempting zero-copy
            waitForDataAndTryZeroCopy(base_message_.size, handler);
        } else {
            // LOG(DEBUG, LOG_TAG) << "Message size " << base_message_.size << " bytes not suitable for zero-copy (min=" << MIN_ZEROCOPY_SIZE << ", page=" << PAGE_SIZE << ")\n";
            // Step 3: Fallback to regular async_read for message body
            receiveRegular(base_message_.size, handler);
        }
    });
}

void ClientConnectionTcpZeroCopy::waitForDataAndTryZeroCopy(size_t expected_size, const MessageHandler<msg::BaseMessage>& handler)
{
    // Wait for socket to have data ready for reading
    socket_.async_wait(tcp_socket::wait_read, [this, expected_size, handler](const boost::system::error_code& ec)
    {
        if (ec) {
            LOG(ERROR, LOG_TAG) << "Error waiting for socket data availability: " << ec.message() << "\n";
            if (handler)
                handler(ec, nullptr);
            return;
        }

        LOG(DEBUG, LOG_TAG) << "Socket has data available, attempting TCP_ZEROCOPY_RECEIVE for " << expected_size << " bytes\n";

        // Now that data is available, try zero-copy receive
        if (tryZeroCopyReceive(expected_size, handler)) {
            return; // Zero-copy successful
        }

        LOG(DEBUG, LOG_TAG) << "TCP_ZEROCOPY_RECEIVE failed even with data available, falling back to regular receive\n";

        // Fall back to regular receive
        receiveRegular(expected_size, handler);
    });
}

bool ClientConnectionTcpZeroCopy::tryZeroCopyReceive(size_t expected_size, const MessageHandler<msg::BaseMessage>& handler)
{
    stats_.zerocopy_attempts++;

    try {
        // Get native socket handle
        int native_socket = getNativeSocket();
        if (native_socket < 0) {
            LOG(ERROR, LOG_TAG) << "Failed to get native socket handle\n";
            return false;
        }

        // Round size to page boundary as required by TCP_ZEROCOPY_RECEIVE
        size_t rounded_size = MmapBufferPool::roundToPageSize(expected_size);

        // Map the socket directly for zero-copy receive (not anonymous mapping!)
        void* mapped_data = mmap(nullptr, rounded_size, PROT_READ, MAP_SHARED, native_socket, 0);

        if (mapped_data == MAP_FAILED) {
            LOG(DEBUG, LOG_TAG) << "mmap failed for socket " << native_socket << ": " << strerror(errno) << "\n";
            stats_.mmap_buffer_misses++;
            return false;
        }

        LOG(DEBUG, LOG_TAG) << "Successfully mapped " << rounded_size << " bytes at " << mapped_data << " for socket " << native_socket << "\n";
        stats_.mmap_buffer_hits++;

        // Prepare TCP_ZEROCOPY_RECEIVE structure
        struct tcp_zerocopy_receive zc = {};
        zc.address = reinterpret_cast<uint64_t>(mapped_data);
        zc.length = static_cast<uint32_t>(rounded_size);
        zc.recv_skip_hint = 0; // Initialize to 0

        LOG(DEBUG, LOG_TAG) << "Calling getsockopt TCP_ZEROCOPY_RECEIVE with address=" << std::hex << zc.address << std::dec << ", length=" << zc.length << "\n";

        socklen_t optlen = sizeof(zc);
        int ret = getsockopt(native_socket, IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE, &zc, &optlen);

        if (ret == 0 && zc.length > 0) {
            // Zero-copy successful!
            stats_.zerocopy_successful++;
            stats_.zerocopy_bytes += zc.length;

            LOG(DEBUG, LOG_TAG) << "Zero-copy receive successful: " << zc.length << " bytes mapped, skip_hint=" << zc.recv_skip_hint << "\n";

            // Handle recv_skip_hint if needed
            if (zc.recv_skip_hint > 0) {
                LOG(DEBUG, LOG_TAG) << "recv_skip_hint=" << zc.recv_skip_hint << " bytes need conventional read\n";
                // For now, fall back if we have skip hint - we can implement this later
                munmap(mapped_data, rounded_size);
                return false;
            }

            // Process the zero-copy data
            processZeroCopyData(mapped_data, zc.length, handler);

            // Unmap after processing
            munmap(mapped_data, rounded_size);
            return true;
        } else {
            // Zero-copy failed, clean up and fall back
            if (ret != 0) {
                LOG(DEBUG, LOG_TAG) << "TCP_ZEROCOPY_RECEIVE failed: " << strerror(errno) << " (ret=" << ret << ")\n";
            } else {
                LOG(DEBUG, LOG_TAG) << "TCP_ZEROCOPY_RECEIVE returned 0 bytes (length=" << zc.length << ", skip_hint=" << zc.recv_skip_hint << ")\n";
            }
            munmap(mapped_data, rounded_size);
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

    // Use buffer pool for regular message body receive
    auto body_buffer_guard = buffer_pool_.acquire(message_size);
    auto& body_buffer = body_buffer_guard.get();

    boost::asio::async_read(socket_, boost::asio::buffer(body_buffer.data(), message_size),
                           [this, handler, body_buffer_guard = std::move(body_buffer_guard)](boost::system::error_code ec, std::size_t length) mutable
    {
        if (ec)
        {
            LOG(ERROR, LOG_TAG) << "Error reading message body of length " << length << ": " << ec.message() << "\n";
            if (handler)
                handler(ec, nullptr);
            return;
        }

        auto response = msg::factory::createMessage(base_message_, body_buffer_guard.get().data());
        if (!response)
            LOG(WARNING, LOG_TAG) << "Failed to deserialize message of type: " << base_message_.type << "\n";

        messageReceived(std::move(response), handler);
        // body_buffer_guard automatically returns buffer to pool when it goes out of scope
    });
}

bool ClientConnectionTcpZeroCopy::isSuitableForZeroCopy(size_t size) const
{
    // Must be at least minimum size to justify zero-copy overhead
    // No need to check page alignment - MmapBufferPool handles rounding up
    return size >= MIN_ZEROCOPY_SIZE;
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
        // Create message from zero-copy mapped data using the actual protocol
        auto response = msg::factory::createMessage(base_message_, static_cast<char*>(mapped_data));
        if (!response) {
            LOG(WARNING, LOG_TAG) << "Failed to deserialize zero-copy message of type: " << base_message_.type << "\n";
            handler(boost::asio::error::invalid_argument, nullptr);
            return;
        }

        LOG(DEBUG, LOG_TAG) << "Processed zero-copy data: " << data_size << " bytes for message type " << base_message_.type << "\n";

        messageReceived(std::move(response), handler);

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
    if (mmap_buffer_pool_)
        mmap_buffer_pool_->logStats();
}
