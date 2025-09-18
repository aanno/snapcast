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
#include "common/buffer_pool.hpp"
#include "common/message/codec_header.hpp"
#include "common/message/factory.hpp"
#include "common/str_compat.hpp"

// 3rd party headers
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

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
    : ClientConnection(io_context, std::move(server))
    , stats_timer_(strand_)
    , socket_(strand_)
    , buffer_pool_(DynamicBufferPool::instance())
{
    LOG(INFO, LOG_TAG) << "Zero-Copy TCP connection initialized (" 
                       << (server_.zerocopy ? "enabled" : "disabled") 
                       << ") with page size: " << PAGE_SIZE << " bytes\n";
    if (server_.zerocopy) {
        LOG(INFO, LOG_TAG) << "Zero-copy receive enabled with iterative TCP_ZEROCOPY_RECEIVE approach\n";
    } else {
        LOG(INFO, LOG_TAG) << "Zero-copy disabled: will use regular async_read for all messages\n";
    }
    initStatsLogging();
}

ClientConnectionTcpZeroCopy::~ClientConnectionTcpZeroCopy()
{
    disconnect();
}

void ClientConnectionTcpZeroCopy::disconnect()
{
    LOG(DEBUG, LOG_TAG) << "Disconnecting zero-copy client\n";
    stats_timer_.cancel();
    
    if (!socket_.is_open())
    {
        LOG(DEBUG, LOG_TAG) << "Not connected\n";
        return;
    }
    
    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    if (ec)
        LOG(ERROR, LOG_TAG) << "Error in socket shutdown: " << ec.message() << "\n";
    socket_.close(ec);
    if (ec)
        LOG(ERROR, LOG_TAG) << "Error in socket close: " << ec.message() << "\n";

    cancelRequests();
    LOG(DEBUG, LOG_TAG) << "Zero-copy connection disconnected\n";
}

void ClientConnectionTcpZeroCopy::getNextMessage(const MessageHandler<msg::BaseMessage>& handler)
{
    // Initialize MmapBufferPool on first use if not already done
    if (!mmap_buffer_pool_) {
        int native_socket = getNativeSocket();
        if (native_socket >= 0) {
            try {
                mmap_buffer_pool_ = std::make_unique<MmapBufferPool>(4, native_socket);
                LOG(DEBUG, LOG_TAG) << "Initialized MmapBufferPool for socket " << native_socket << "\n";
            } catch (const std::exception& e) {
                LOG(ERROR, LOG_TAG) << "Failed to initialize MmapBufferPool: " << e.what() << "\n";
                // Continue without MmapBufferPool - this will cause fallback behavior
            }
        } else {
            LOG(ERROR, LOG_TAG) << "Invalid native socket handle: " << native_socket << "\n";
        }
    }

    // ==== SEQUENTIAL CONTROLLED LOOP: Only ONE header read at a time ====

    // Queue the handler for processing
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        pending_handlers_.push(handler);
    }

    // Start a controlled read ONLY if none are active (ensure sequential processing)
    size_t current_active = active_reads_.load();
    if (current_active == 0) {
        // Atomically set to 1 if currently 0 (only one reader allowed)
        size_t expected = 0;
        if (active_reads_.compare_exchange_weak(expected, 1)) {
            LOG(DEBUG, LOG_TAG) << "Starting sequential controlled read\n";
            startControlledRead();
        }
    } else {
        LOG(DEBUG, LOG_TAG) << "Sequential read in progress, handler queued\n";
    }
}

// ============ Controlled Async Loop Implementation ============

void ClientConnectionTcpZeroCopy::startControlledRead()
{
    LOG(DEBUG, LOG_TAG) << "startControlledRead: Beginning header read\n";

    // Step 1: Read message header using DynamicBufferPool (regular boost::asio I/O)
    auto header_buffer_guard = buffer_pool_.acquire(base_msg_size_);
    auto& header_buffer = header_buffer_guard.get();

    boost::asio::async_read(socket_, boost::asio::buffer(header_buffer.data(), base_msg_size_),
                           boost::asio::bind_executor(strand_, [this, header_buffer_guard = std::move(header_buffer_guard)](boost::system::error_code ec, std::size_t length) mutable
    {
        LOG(DEBUG, LOG_TAG) << "startControlledRead: Header read completed, ec=" << ec << ", length=" << length << "\n";

        try {
            LOG(DEBUG, LOG_TAG) << "TRACE: Entering header completion handler, ec.value()=" << ec.value() << "\n";

            if (ec)
            {
                LOG(DEBUG, LOG_TAG) << "TRACE: Error condition detected, calling onControlledReadComplete\n";
            LOG(ERROR, LOG_TAG) << "Error reading message header of length " << length << ": " << ec.message() << "\n";
            // Complete this controlled read with error
            onControlledReadComplete(ec, nullptr);
            return;
        }

        LOG(DEBUG, LOG_TAG) << "TRACE: No error, proceeding to parse header\n";

        // Parse header
        LOG(DEBUG, LOG_TAG) << "TRACE: About to deserialize header\n";
        base_message_.deserialize(header_buffer_guard.get().data());
        LOG(DEBUG, LOG_TAG) << "TRACE: Header deserialized, type=" << base_message_.type << ", size=" << base_message_.size << "\n";
        tv t;
        base_message_.received = t;

        LOG(DEBUG, LOG_TAG) << "TRACE: About to validate message type and size\n";

        if (base_message_.type > message_type::kLast)
        {
            LOG(ERROR, LOG_TAG) << "unknown message type received: " << base_message_.type << ", size: " << base_message_.size << "\n";
            onControlledReadComplete(boost::asio::error::invalid_argument, nullptr);
            return;
        }
        else if (base_message_.size > msg::max_size)
        {
            LOG(ERROR, LOG_TAG) << "received message of type " << base_message_.type << " too large: " << base_message_.size << "\n";
            onControlledReadComplete(boost::asio::error::invalid_argument, nullptr);
            return;
        }

        LOG(DEBUG, LOG_TAG) << "TRACE: Message validation passed, checking zerocopy flag\n";

        // Step 2: Short-circuit complex zero-copy logic when -z flag not used
        if (!server_.zerocopy) {
            LOG(DEBUG, LOG_TAG) << "TRACE: -z flag not set, using receiveRegular\n";
            // -z flag not set: skip zero-copy entirely, use regular receive
            receiveRegular(base_message_.size, [this](const boost::system::error_code& ec, std::unique_ptr<msg::BaseMessage> response) {
                onControlledReadComplete(ec, std::move(response));
            });
            return;
        }

        LOG(DEBUG, LOG_TAG) << "TRACE: -z flag is set, checking if suitable for zerocopy\n";

        // Step 3: For message body, try TCP_ZEROCOPY_RECEIVE if suitable and enabled
        if (isSuitableForZeroCopy(base_message_.size)) {
            LOG(DEBUG, LOG_TAG) << "TRACE: Message suitable for zerocopy, calling waitForDataAndTryZeroCopy\n";
            // Wait for socket to have data ready before attempting zero-copy
            waitForDataAndTryZeroCopy(base_message_.size, [this](const boost::system::error_code& ec, std::unique_ptr<msg::BaseMessage> response) {
                onControlledReadComplete(ec, std::move(response));
            });
        } else {
            LOG(DEBUG, LOG_TAG) << "TRACE: Message not suitable for zerocopy, calling receiveRegular fallback\n";
            // Step 4: Fallback to regular async_read for message body
            receiveRegular(base_message_.size, [this](const boost::system::error_code& ec, std::unique_ptr<msg::BaseMessage> response) {
                onControlledReadComplete(ec, std::move(response));
            });
        }

        } catch (const std::exception& e) {
            LOG(ERROR, LOG_TAG) << "TRACE: Exception in header completion handler: " << e.what() << "\n";
            onControlledReadComplete(boost::asio::error::operation_aborted, nullptr);
        } catch (...) {
            LOG(ERROR, LOG_TAG) << "TRACE: Unknown exception in header completion handler\n";
            onControlledReadComplete(boost::asio::error::operation_aborted, nullptr);
        }
    }));
}

void ClientConnectionTcpZeroCopy::onControlledReadComplete(const boost::system::error_code& ec, std::unique_ptr<msg::BaseMessage> response)
{
    LOG(DEBUG, LOG_TAG) << "onControlledReadComplete: ec=" << ec << ", response=" << (response ? "valid" : "null") << "\n";

    // Get the next handler from the queue FIRST
    MessageHandler<msg::BaseMessage> handler;
    bool has_more_pending = false;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        if (!pending_handlers_.empty()) {
            handler = pending_handlers_.front();
            pending_handlers_.pop();
            has_more_pending = !pending_handlers_.empty();
        }
    }

    // Call the handler if we have one
    if (handler) {
        handler(ec, std::move(response));
    }

    // CONTINUOUS SEQUENTIAL PROCESSING: Keep reading as long as socket is connected
    // Reset active_reads to 0, then start next read if no error
    active_reads_.store(0);

    if (ec) {
        // Stop reading on error
        LOG(DEBUG, LOG_TAG) << "Sequential reading stopped due to error: " << ec.message() << "\n";
    } else {
        // Continue reading - either for pending handlers or to receive server messages
        // Use boost::asio::post to defer to next event loop iteration (threading fix!)
        boost::asio::post(strand_, [this, has_more_pending]() {
            size_t expected = 0;
            if (active_reads_.compare_exchange_weak(expected, 1)) {
                if (has_more_pending) {
                    LOG(DEBUG, LOG_TAG) << "Starting next sequential controlled read (pending handlers)\n";
                } else {
                    LOG(DEBUG, LOG_TAG) << "Starting next sequential controlled read (continuous)\n";
                }
                startControlledRead();
            }
        });
    }
}

void ClientConnectionTcpZeroCopy::waitForDataAndTryZeroCopy(size_t expected_size, const MessageHandler<msg::BaseMessage>& handler)
{
    // Wait for socket to have data ready for reading
    socket_.async_wait(tcp_socket::wait_read, boost::asio::bind_executor(strand_, [this, expected_size, handler](const boost::system::error_code& ec)
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

        // Track fallback reason - could be size or page alignment issues
        if (expected_size < MIN_ZEROCOPY_SIZE) {
            stats_.fallback_size_mismatch++;
        } else {
            stats_.fallback_page_misalign++;
        }

        // Fall back to regular receive
        receiveRegular(expected_size, handler);
    }));
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

        // Initialize MmapBufferPool on first use (thread-safe, lazy initialization)
        if (!mmap_buffer_pool_) {
            mmap_buffer_pool_ = std::make_unique<MmapBufferPool>(4, native_socket);
            LOG(DEBUG, LOG_TAG) << "Initialized MmapBufferPool for socket " << native_socket << "\n";
        }

        // Implement iterative approach for TCP_ZEROCOPY_RECEIVE
        size_t total_bytes_received = 0;
        size_t remaining_bytes = expected_size;
        std::vector<char> message_buffer;
        message_buffer.reserve(expected_size);
        
        while (remaining_bytes > 0) {
            // Acquire page-aligned buffer from pool
            auto buffer_guard = mmap_buffer_pool_->acquire(std::max(remaining_bytes, PAGE_SIZE));
            if (!buffer_guard) {
                LOG(DEBUG, LOG_TAG) << "MmapBufferPool.acquire failed for " << remaining_bytes << " bytes\n";
                return false;
            }

            // Prepare TCP_ZEROCOPY_RECEIVE structure
            struct tcp_zerocopy_receive zc = {};
            zc.address = reinterpret_cast<uint64_t>(buffer_guard->data());
            zc.length = static_cast<uint32_t>(buffer_guard->size());
            zc.recv_skip_hint = 0;

            LOG(DEBUG, LOG_TAG) << "TCP_ZEROCOPY_RECEIVE iteration: expecting " << remaining_bytes 
                               << " bytes, buffer size " << buffer_guard->size() << "\n";

            socklen_t optlen = sizeof(zc);
            int ret = getsockopt(native_socket, IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE, &zc, &optlen);

            if (ret == 0 && zc.length > 0) {
                // Zero-copy successful for this iteration
                LOG(DEBUG, LOG_TAG) << "Zero-copy received " << zc.length << " bytes, skip_hint=" << zc.recv_skip_hint << "\n";
                
                // Copy zero-copy data to message buffer
                size_t bytes_to_copy = std::min(static_cast<size_t>(zc.length), remaining_bytes);
                message_buffer.insert(message_buffer.end(), 
                                     static_cast<const char*>(buffer_guard->data()),
                                     static_cast<const char*>(buffer_guard->data()) + bytes_to_copy);
                
                total_bytes_received += bytes_to_copy;
                remaining_bytes -= bytes_to_copy;
                
                stats_.zerocopy_bytes += bytes_to_copy;
                
                // Handle recv_skip_hint (conventional read needed)
                if (zc.recv_skip_hint > 0) {
                    LOG(DEBUG, LOG_TAG) << "recv_skip_hint=" << zc.recv_skip_hint 
                                       << " bytes need conventional recv()\n";
                    
                    // Use conventional recv() for skip_hint bytes
                    size_t skip_bytes_to_read = std::min(static_cast<size_t>(zc.recv_skip_hint), remaining_bytes);
                    if (skip_bytes_to_read > 0) {
                        std::vector<char> skip_buffer(skip_bytes_to_read);
                        ssize_t bytes_read = recv(native_socket, skip_buffer.data(), skip_bytes_to_read, 0);
                        
                        if (bytes_read > 0) {
                            message_buffer.insert(message_buffer.end(), 
                                                 skip_buffer.begin(), 
                                                 skip_buffer.begin() + bytes_read);
                            total_bytes_received += bytes_read;
                            remaining_bytes -= bytes_read;
                            LOG(DEBUG, LOG_TAG) << "Conventional recv() got " << bytes_read << " bytes\n";
                        } else if (bytes_read < 0) {
                            LOG(ERROR, LOG_TAG) << "recv() failed: " << strerror(errno) << "\n";
                            return false;
                        }
                    }
                }
                
                // Check if we've received the complete message
                if (total_bytes_received >= expected_size) {
                    break;
                }
                
            } else {
                // Zero-copy failed for this iteration
                if (ret != 0) {
                    LOG(DEBUG, LOG_TAG) << "TCP_ZEROCOPY_RECEIVE failed: " << strerror(errno) << "\n";
                } else {
                    LOG(DEBUG, LOG_TAG) << "TCP_ZEROCOPY_RECEIVE returned 0 bytes (no data available)\n";
                }
                return false;
            }
        }
        
        // Process the complete message
        if (total_bytes_received >= expected_size) {
            stats_.zerocopy_successful++;
            processZeroCopyData(message_buffer.data(), expected_size, handler);
            return true;
        } else {
            LOG(WARNING, LOG_TAG) << "Incomplete zero-copy receive: " << total_bytes_received 
                                 << "/" << expected_size << " bytes\n";
            return false;
        }

    } catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Exception in zero-copy receive: " << e.what() << "\n";
        return false;
    }
}

void ClientConnectionTcpZeroCopy::receiveRegular(size_t message_size, const MessageHandler<msg::BaseMessage>& handler)
{
    LOG(DEBUG, LOG_TAG) << "TRACE: receiveRegular called for size=" << message_size << "\n";

    stats_.regular_receives++;
    stats_.regular_bytes += message_size;

    // Use DynamicBufferPool for regular message body receive (boost::asio I/O)
    auto body_buffer_guard = buffer_pool_.acquire(message_size);
    auto& body_buffer = body_buffer_guard.get();

    LOG(DEBUG, LOG_TAG) << "TRACE: receiveRegular starting async_read for body\n";

    boost::asio::async_read(socket_, boost::asio::buffer(body_buffer.data(), message_size),
                           boost::asio::bind_executor(strand_, [this, handler, body_buffer_guard = std::move(body_buffer_guard)](boost::system::error_code ec, std::size_t length) mutable
    {
        LOG(DEBUG, LOG_TAG) << "TRACE: receiveRegular completion handler called, ec=" << ec << ", length=" << length << "\n";
        if (ec)
        {
            LOG(ERROR, LOG_TAG) << "Error reading message body of length " << length << ": " << ec.message() << "\n";
            if (handler)
                handler(ec, nullptr);
            return;
        }

        LOG(DEBUG, LOG_TAG) << "TRACE: receiveRegular creating message from body data\n";

        auto response = msg::factory::createMessage(base_message_, body_buffer_guard.get().data());
        if (!response)
            LOG(WARNING, LOG_TAG) << "Failed to deserialize message of type: " << base_message_.type << "\n";

        LOG(DEBUG, LOG_TAG) << "TRACE: receiveRegular calling messageReceived and handler\n";
        messageReceived(std::move(response), handler);
        // body_buffer_guard automatically returns buffer to pool when it goes out of scope
    }));
}

void ClientConnectionTcpZeroCopy::messageReceived(std::unique_ptr<msg::BaseMessage> message, const MessageHandler<msg::BaseMessage>& handler)
{
    LOG(DEBUG, LOG_TAG) << "TRACE: messageReceived override called with handler, message type: " << message->type << "\n";

    // Handle special messages for Controller integration
    if (message->type == message_type::kServerSettings && server_settings_handler_) {
        LOG(DEBUG, LOG_TAG) << "TRACE: Received ServerSettings, calling Controller callback\n";
        // Create a copy for the callback since we need to pass message to handler too
        auto server_settings = dynamic_cast<msg::ServerSettings*>(message.get());
        if (server_settings) {
            auto settings_copy = std::make_unique<msg::ServerSettings>(*server_settings);
            server_settings_handler_(std::move(settings_copy));
        }
    } else if (message->type == message_type::kTime && time_handler_) {
        LOG(DEBUG, LOG_TAG) << "TRACE: Received Time response, calling Controller callback\n";
        // Create a copy for the callback since we need to pass message to handler too
        auto time_msg = dynamic_cast<msg::Time*>(message.get());
        if (time_msg) {
            auto time_copy = std::make_unique<msg::Time>(*time_msg);
            time_handler_(std::move(time_copy));
        }
    }

    // For controlled reading, we ALWAYS call the handler to maintain the sequential pattern
    // Don't call getNextMessage recursively like the parent does - our controlled reading handles that
    if (handler) {
        LOG(DEBUG, LOG_TAG) << "TRACE: calling handler with received message\n";
        handler({}, std::move(message));
    } else {
        LOG(WARNING, LOG_TAG) << "TRACE: handler is null in messageReceived\n";
    }
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
    LOG(INFO, LOG_TAG) << "\tZC Success Rate: " << std::fixed << std::setprecision(2) << stats_.getSuccessRate() << "%\n";

    // Log buffer pool stats
    LOG(INFO, LOG_TAG) << "=== Buffer Pool Stats ===\n";
    auto dynamic_stats = buffer_pool_.getStats();
    LOG(INFO, LOG_TAG) << "\tDynamic Pool - Total Buffers: " << dynamic_stats.total_buffers << "\n";
    LOG(INFO, LOG_TAG) << "\tDynamic Pool - Available: " << dynamic_stats.available_buffers << "\n";
    LOG(INFO, LOG_TAG) << "\tDynamic Pool - Bytes Allocated: " << dynamic_stats.bytes_allocated << "\n";
    LOG(INFO, LOG_TAG) << "\tDynamic Pool - Created: " << dynamic_stats.buffers_created << "\n";
    LOG(INFO, LOG_TAG) << "\tDynamic Pool - Reused: " << dynamic_stats.buffers_reused << "\n";
    LOG(INFO, LOG_TAG) << "\tDynamic Pool - Cleanup Ops: " << dynamic_stats.cleanup_operations << "\n";
    LOG(INFO, LOG_TAG) << "\tDynamic Pool - Potential Leaks (>10s): " << dynamic_stats.potential_leaks << "\n";

    if (mmap_buffer_pool_)
        mmap_buffer_pool_->logStats();
}

std::string ClientConnectionTcpZeroCopy::getMacAddress()
{
    std::string mac =
#ifndef WINDOWS
        ::getMacAddress(socket_.native_handle());
#else
        ::getMacAddress(socket_.local_endpoint().address().to_string());
#endif
    if (mac.empty())
        mac = "00:00:00:00:00:00";
    LOG(INFO, LOG_TAG) << "My MAC: \"" << mac << "\", socket: " << socket_.native_handle() << "\n";
    return mac;
}

boost::system::error_code ClientConnectionTcpZeroCopy::doConnect(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint)
{
    boost::system::error_code ec;
    socket_.connect(endpoint, ec);
    return ec;
}

void ClientConnectionTcpZeroCopy::write(boost::asio::streambuf& buffer, WriteHandler&& write_handler)
{
    boost::asio::async_write(socket_, buffer, write_handler);
}
