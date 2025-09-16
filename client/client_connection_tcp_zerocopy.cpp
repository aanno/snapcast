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

// prototype/interface header file
#include "client_connection_tcp_zerocopy.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/message/factory.hpp"

// 3rd party headers
#include <boost/asio/read.hpp>

// standard headers
#include <iomanip>
#include <cstring>

// MSG_ZEROCOPY definition for compatibility with older headers
#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

static constexpr auto LOG_TAG = "ClientConnectionZeroCopy";
static constexpr auto LOG_TAG_STATS = "ClientZeroCopyStats";

ClientConnectionTcpZeroCopy::ClientConnectionTcpZeroCopy(boost::asio::io_context& io_context, ClientSettings::Server server)
    : ClientConnectionTcp(io_context, std::move(server)), stats_timer_(strand_), buffer_pool_(DynamicBufferPool::instance())
{
    LOG(INFO, LOG_TAG) << "Creating zero-copy TCP client connection for RECEIVE\\n";
}

ClientConnectionTcpZeroCopy::~ClientConnectionTcpZeroCopy()
{
    disconnect();
    
    // Log final statistics
    if (zerocopy_available_)
    {
        auto stats = getZeroCopyStats();
        LOG(INFO, LOG_TAG) << "Final zero-copy receive stats - ZC: " << stats.zerocopy_successful 
                           << "/" << stats.zerocopy_attempts << " (" << std::fixed << std::setprecision(2) 
                           << stats.zerocopy_percentage() << "%), Regular: " << stats.regular_receives << "\\n";
        
        auto buffer_stats = buffer_pool_.getStats();
        LOG(INFO, LOG_TAG) << "Final buffer pool stats - Total: " << buffer_stats.total_buffers
                           << ", Available: " << buffer_stats.available_buffers
                           << ", Reused: " << buffer_stats.buffers_reused << "\\n";
    }
}

void ClientConnectionTcpZeroCopy::disconnect()
{
    stopPeriodicLogging();
    ClientConnectionTcp::disconnect();
}

bool ClientConnectionTcpZeroCopy::initializeZeroCopy()
{
    native_socket_ = socket_.native_handle();
    LOG(DEBUG, LOG_TAG) << "Native socket handle: " << native_socket_ << "\\n";
    
    if (native_socket_ < 0)
    {
        LOG(WARNING, LOG_TAG) << "Invalid native socket handle: " << native_socket_ << "\\n";
        return false;
    }
    
    // For receive zero-copy, we use direct recv() to avoid boost::asio buffer copies
    // This is different from server's MSG_ZEROCOPY which is for send operations
    
    LOG(INFO, LOG_TAG) << "Zero-copy receive capability initialized for client\\n";
    return true;
}

// SERVER COMPLIANCE: Atomic coordination methods
bool ClientConnectionTcpZeroCopy::tryReserveZeroCopy()
{
    // SERVER COMPLIANCE: Atomically check if idle and reserve for zerocopy (race-condition safe)
    uint32_t expected = 0;
    while (!pending_async_operations_.compare_exchange_weak(expected, 1))
    {
        if (expected != 0)
        {
            // Another operation is in progress; cannot do zerocopy now
            return false;
        }
        // If spurious failure, 'expected' is reloaded with current value, retry
    }
    return true; // Successfully reserved zerocopy
}

void ClientConnectionTcpZeroCopy::releaseZeroCopy()
{
    // Release zerocopy reservation
    pending_async_operations_--;
}

bool ClientConnectionTcpZeroCopy::canUseZeroCopy() const
{
    return pending_async_operations_.load() == 0;
}

void ClientConnectionTcpZeroCopy::getNextMessage(const MessageHandler<msg::BaseMessage>& handler)
{
    // Initialize zero-copy on first message receive if not already done
    if (!zerocopy_available_ && socket_.is_open())
    {
        zerocopy_available_ = initializeZeroCopy();
        if (zerocopy_available_)
        {
            startPeriodicLogging();
            LOG(INFO, LOG_TAG) << "Zero-copy receive enabled, starting periodic logging\\n";
        }
        else
        {
            LOG(WARNING, LOG_TAG) << "Zero-copy receive initialization failed, using regular receive\\n";
        }
    }
    
    // First, read the message header using regular async_read (headers are small)
    boost::asio::async_read(socket_, boost::asio::buffer(buffer_, base_msg_size_), 
                           [this, handler](boost::system::error_code ec, std::size_t length) mutable
    {
        if (ec)
        {
            LOG(ERROR, LOG_TAG) << "Error reading message header of length " << length << ": " << ec.message() << "\\n";
            if (handler)
                handler(ec, nullptr);
            return;
        }

        base_message_.deserialize(buffer_.data());
        tv t;
        base_message_.received = t;
        
        if (base_message_.type > message_type::kLast)
        {
            LOG(ERROR, LOG_TAG) << "unknown message type received: " << base_message_.type << ", size: " << base_message_.size << "\\n";
            if (handler)
                handler(boost::asio::error::invalid_argument, nullptr);
            return;
        }
        else if (base_message_.size > msg::max_size)
        {
            LOG(ERROR, LOG_TAG) << "received message of type " << base_message_.type << " too large: " << base_message_.size << "\\n";
            if (handler)
                handler(boost::asio::error::invalid_argument, nullptr);
            return;
        }

        // SERVER COMPLIANCE: Coordinated decision for zero-copy receive
        if (zerocopy_available_ && base_message_.size >= ZEROCOPY_THRESHOLD)
        {
            // LOG(DEBUG, LOG_TAG) << "Attempting coordinated zero-copy receive for " << base_message_.size << " byte message\\n";
            if (tryZeroCopyReceive(base_message_.size, handler))
            {
                return; // Zero-copy receive initiated
            }
            else
            {
                LOG(DEBUG, LOG_TAG) << "Zero-copy receive failed, falling back to regular receive\\n";
                coordination_fallbacks_++;
            }
        }
        
        // Fall back to regular async_read for small messages or when zero-copy fails
        receiveRegularCoordinated(base_message_.size, handler);
    });
}

bool ClientConnectionTcpZeroCopy::tryZeroCopyReceive(size_t message_size, const MessageHandler<msg::BaseMessage>& handler)
{
    // SERVER COMPLIANCE: Try to reserve zero-copy access atomically
    if (!tryReserveZeroCopy())
    {
        // Cannot use zero-copy right now due to pending async operations
        return false;
    }
    
    // Now we have exclusive access for zero-copy receive
    receiveZeroCopyCoordinated(message_size, handler);
    return true;
}

void ClientConnectionTcpZeroCopy::receiveZeroCopyCoordinated(size_t message_size, const MessageHandler<msg::BaseMessage>& handler)
{
    zerocopy_attempts_++;
    
    // Ensure we have a large enough zero-copy buffer
    if (!zerocopy_buffer_ || zerocopy_buffer_size_ < message_size)
    {
        zerocopy_buffer_ = std::make_unique<char[]>(message_size);
        zerocopy_buffer_size_ = message_size;
    }
    
    // Use direct recv with zero-copy buffer (avoiding boost::asio buffer copy)
    // This reduces one memory copy compared to boost::asio::async_read
    ssize_t result = recv(native_socket_, zerocopy_buffer_.get(), message_size, MSG_DONTWAIT);
    
    if (result < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Would block - fall back to regular async receive
            LOG(TRACE, LOG_TAG) << "Zero-copy receive would block, falling back to regular receive\\n";
            releaseZeroCopy();
            receiveRegularCoordinated(message_size, handler);
            return;
        }
        else
        {
            LOG(ERROR, LOG_TAG) << "Zero-copy recv failed: " << strerror(errno) << "\\n";
            releaseZeroCopy();
            receiveRegularCoordinated(message_size, handler);
            return;
        }
    }
    
    if (static_cast<size_t>(result) != message_size)
    {
        LOG(WARNING, LOG_TAG) << "Zero-copy partial receive: " << result << "/" << message_size << " bytes\\n";
        // For partial receives, we'd need more complex handling
        // For now, fall back to regular receive
        releaseZeroCopy();
        receiveRegularCoordinated(message_size, handler);
        return;
    }
    
    // Success - zero-copy receive completed
    zerocopy_successful_++;
    zerocopy_bytes_ += message_size;
    outstanding_zerocopy_buffers_++;
    
    // LOG(TRACE, LOG_TAG) << "Zero-copy receive successful: " << message_size << " bytes\\n";
    
    // Process the received message
    auto response = msg::factory::createMessage(base_message_, zerocopy_buffer_.get());
    if (!response)
        LOG(WARNING, LOG_TAG) << "Failed to deserialize message of type: " << base_message_.type << "\\n";

    // Release zero-copy reservation and call handler
    releaseZeroCopy();
    outstanding_zerocopy_buffers_--;
    
    // Schedule the handler to be called
    boost::asio::post(strand_, [this, handler, response = std::move(response)]() mutable
    {
        messageReceived(std::move(response), handler);
    });
}

void ClientConnectionTcpZeroCopy::receiveRegularCoordinated(size_t message_size, const MessageHandler<msg::BaseMessage>& handler)
{
    // SERVER COMPLIANCE: Track regular async operations
    pending_async_operations_++;
    regular_receives_++;
    regular_bytes_ += message_size;
    
    LOG(DEBUG, LOG_TAG) << "Regular receive started for " << message_size << " bytes, pending_async_operations now: " << pending_async_operations_.load() << "\\n";
    
    if (message_size > buffer_.size())
        buffer_.resize(message_size);

    boost::asio::async_read(socket_, boost::asio::buffer(buffer_, message_size),
                           [this, handler](boost::system::error_code ec, std::size_t length) mutable
    {
        // SERVER COMPLIANCE: Decrement coordination counter
        pending_async_operations_--;
        LOG(DEBUG, LOG_TAG) << "Regular receive completed: " << length << " bytes, pending_async_operations now: " << pending_async_operations_.load() << "\\n";
        
        if (ec)
        {
            LOG(ERROR, LOG_TAG) << "Error reading message body of length " << length << ": " << ec.message() << "\\n";
            if (handler)
                handler(ec, nullptr);
            return;
        }

        auto response = msg::factory::createMessage(base_message_, buffer_.data());
        if (!response)
            LOG(WARNING, LOG_TAG) << "Failed to deserialize message of type: " << base_message_.type << "\\n";

        messageReceived(std::move(response), handler);
    });
}

void ClientConnectionTcpZeroCopy::startPeriodicLogging()
{
    if (logging_active_.load())
        return;
        
    logging_active_.store(true);
    scheduleNextStatisticsLog();
    
    LOG(DEBUG, LOG_TAG_STATS) << "Started periodic statistics logging\\n";
}

void ClientConnectionTcpZeroCopy::stopPeriodicLogging()
{
    if (!logging_active_.load())
        return;
        
    logging_active_.store(false);
    stats_timer_.cancel();
    
    LOG(DEBUG, LOG_TAG_STATS) << "Stopped periodic statistics logging\\n";
}

void ClientConnectionTcpZeroCopy::scheduleNextStatisticsLog()
{
    if (!logging_active_.load())
        return;
        
    stats_timer_.expires_after(STATS_LOG_INTERVAL);
    stats_timer_.async_wait([this](const boost::system::error_code& ec)
    {
        if (!ec && logging_active_.load())
        {
            logStatistics();
            scheduleNextStatisticsLog();
        }
    });
}

void ClientConnectionTcpZeroCopy::logStatistics()
{
    if (!zerocopy_available_)
        return;
        
    // SERVER COMPLIANCE: Log zero-copy statistics in server format
    auto zc_stats = getZeroCopyStats();
    LOG(INFO, LOG_TAG_STATS) << "=== Client ZeroCopy Receive Status (every 30s) ===\\n";
    LOG(INFO, LOG_TAG_STATS) << "ZC Attempts: " << zc_stats.zerocopy_attempts << ", "
                             << "ZC Successful: " << zc_stats.zerocopy_successful << ", "
                             << "ZC Bytes: " << zc_stats.zerocopy_bytes << ", "
                             << "Regular Receives: " << zc_stats.regular_receives << ", "
                             << "Regular Bytes: " << zc_stats.regular_bytes << ", "
                             << "Coordination Fallbacks: " << zc_stats.coordination_fallbacks << ", "
                             << "Pending Async Operations: " << zc_stats.pending_async_operations << ", "
                             << "Outstanding ZC Buffers: " << zc_stats.outstanding_zerocopy_buffers << ", "
                             << "ZC Success Rate: " << std::fixed << std::setprecision(2) << zc_stats.zerocopy_percentage() << "%, "
                             << "Completion Reliability: " << std::fixed << std::setprecision(2) << zc_stats.completion_reliability() << "%\\n";
    
    // SERVER COMPLIANCE: Log buffer pool statistics
    auto buffer_stats = buffer_pool_.getStats();
    LOG(INFO, LOG_TAG_STATS) << "Buffer pool stats - Total: " << buffer_stats.total_buffers
                             << ", Available: " << buffer_stats.available_buffers
                             << ", Reused: " << buffer_stats.buffers_reused
                             << ", Created: " << buffer_stats.buffers_created
                             << ", Bytes allocated: " << buffer_stats.bytes_allocated << "\\n";
}

ClientConnectionTcpZeroCopy::ZeroCopyStats ClientConnectionTcpZeroCopy::getZeroCopyStats() const
{
    ZeroCopyStats stats;
    stats.zerocopy_attempts = zerocopy_attempts_.load();
    stats.zerocopy_successful = zerocopy_successful_.load();
    stats.zerocopy_bytes = zerocopy_bytes_.load();
    stats.regular_receives = regular_receives_.load();
    stats.regular_bytes = regular_bytes_.load();
    stats.coordination_fallbacks = coordination_fallbacks_.load();
    stats.pending_async_operations = pending_async_operations_.load();
    stats.outstanding_zerocopy_buffers = outstanding_zerocopy_buffers_.load();
    stats.completion_notifications_received = completion_notifications_received_.load();
    stats.completion_notifications_missing = completion_notifications_missing_.load();
    stats.buffers_completed_via_notifications = buffers_completed_via_notifications_.load();
    
    return stats;
}

void ClientConnectionTcpZeroCopy::resetZeroCopyStats()
{
    zerocopy_attempts_.store(0);
    zerocopy_successful_.store(0);
    zerocopy_bytes_.store(0);
    regular_receives_.store(0);
    regular_bytes_.store(0);
    coordination_fallbacks_.store(0);
    completion_notifications_received_.store(0);
    completion_notifications_missing_.store(0);
    buffers_completed_via_notifications_.store(0);
    // Note: outstanding_zerocopy_buffers and pending_async_operations are not reset as they represent current state
}
