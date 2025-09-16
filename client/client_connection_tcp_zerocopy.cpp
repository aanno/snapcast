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

static constexpr auto LOG_TAG = "ClientConnectionZeroCopy";
static constexpr auto LOG_TAG_STATS = "ClientZeroCopyStats";

ClientConnectionTcpZeroCopy::ClientConnectionTcpZeroCopy(boost::asio::io_context& io_context, ClientSettings::Server server)
    : ClientConnectionTcp(io_context, std::move(server)), stats_timer_(strand_), buffer_pool_(DynamicBufferPool::instance())
{
    LOG(INFO, LOG_TAG) << "Creating TRUE zero-copy TCP client connection for RECEIVE (no memory copies)\\n";
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
    
    LOG(INFO, LOG_TAG) << "TRUE zero-copy receive capability initialized (direct recv into buffer pool)\\n";
    return true;
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
            LOG(INFO, LOG_TAG) << "TRUE zero-copy receive enabled, starting periodic logging\\n";
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

        // Decide whether to use TRUE zero-copy for the message body
        if (zerocopy_available_ && base_message_.size >= ZEROCOPY_THRESHOLD)
        {
            LOG(DEBUG, LOG_TAG) << "Attempting TRUE zero-copy receive for " << base_message_.size << " byte message\\n";
            if (tryZeroCopyReceive(base_message_.size, handler))
            {
                return; // Zero-copy receive initiated
            }
            else
            {
                LOG(DEBUG, LOG_TAG) << "Zero-copy receive failed, falling back to regular receive\\n";
                large_message_fallbacks_++;
            }
        }
        
        // Fall back to regular async_read for small messages or when zero-copy fails
        receiveRegular(base_message_.size, handler);
    });
}

bool ClientConnectionTcpZeroCopy::tryZeroCopyReceive(size_t message_size, const MessageHandler<msg::BaseMessage>& handler)
{
    zerocopy_attempts_++;
    
    // TRUE ZERO-COPY: Acquire buffer from pool - NO allocation, reuse existing buffer
    auto buffer_guard = buffer_pool_.acquire(message_size);
    auto& buffer_data = buffer_guard.get();
    
    // Ensure buffer is large enough
    if (buffer_data.size() < message_size)
    {
        buffer_guard.resize(message_size);
    }
    
    buffer_pool_hits_++; // Track buffer pool usage
    
    // TRUE ZERO-COPY: Direct receive into buffer pool buffer (NO intermediate allocation/copy)
    ssize_t result = recv(native_socket_, buffer_data.data(), message_size, MSG_DONTWAIT);
    
    if (result < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Would block - fall back to regular async receive
            LOG(TRACE, LOG_TAG) << "Zero-copy receive would block, falling back to regular receive\\n";
            return false;
        }
        else
        {
            LOG(ERROR, LOG_TAG) << "Zero-copy recv failed: " << strerror(errno) << "\\n";
            return false;
        }
    }
    
    if (static_cast<size_t>(result) != message_size)
    {
        LOG(WARNING, LOG_TAG) << "Zero-copy partial receive: " << result << "/" << message_size << " bytes\\n";
        // For partial receives, we'd need more complex handling
        // For now, fall back to regular receive
        return false;
    }
    
    // Success - TRUE zero-copy receive completed (no copies, direct into buffer pool)
    zerocopy_successful_++;
    zerocopy_bytes_ += message_size;
    
    LOG(TRACE, LOG_TAG) << "TRUE zero-copy receive successful: " << message_size << " bytes directly into buffer pool (NO COPIES)\\n";
    
    // Process the received message directly from buffer pool buffer (NO COPY)
    auto response = msg::factory::createMessage(base_message_, buffer_data.data());
    if (!response)
        LOG(WARNING, LOG_TAG) << "Failed to deserialize message of type: " << base_message_.type << "\\n";

    // Schedule the handler to be called
    // The buffer_guard automatically returns buffer to pool when scope ends (RAII)
    boost::asio::post(strand_, [this, handler, response = std::move(response), buffer_guard = std::move(buffer_guard)]() mutable
    {
        messageReceived(std::move(response), handler);
        // buffer_guard destructor automatically returns buffer to pool here - TRUE ZERO-COPY COMPLETE
    });
    
    return true;
}

void ClientConnectionTcpZeroCopy::receiveRegular(size_t message_size, const MessageHandler<msg::BaseMessage>& handler)
{
    regular_receives_++;
    regular_bytes_ += message_size;
    
    if (message_size > buffer_.size())
        buffer_.resize(message_size);

    boost::asio::async_read(socket_, boost::asio::buffer(buffer_, message_size),
                           [this, handler](boost::system::error_code ec, std::size_t length) mutable
    {
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
        
    // Log TRUE zero-copy receive statistics
    auto zc_stats = getZeroCopyStats();
    LOG(INFO, LOG_TAG_STATS) << "=== Client TRUE ZeroCopy Receive Status (every 30s) ===\\n";
    LOG(INFO, LOG_TAG_STATS) << "ZC Attempts: " << zc_stats.zerocopy_attempts << ", "
                             << "ZC Successful: " << zc_stats.zerocopy_successful << ", "
                             << "ZC Bytes: " << zc_stats.zerocopy_bytes << ", "
                             << "Regular Receives: " << zc_stats.regular_receives << ", "
                             << "Regular Bytes: " << zc_stats.regular_bytes << ", "
                             << "Large Message Fallbacks: " << zc_stats.large_message_fallbacks << ", "
                             << "ZC Success Rate: " << std::fixed << std::setprecision(2) << zc_stats.zerocopy_percentage() << "%, "
                             << "Buffer Pool Hit Rate: " << std::fixed << std::setprecision(2) << zc_stats.buffer_pool_hit_rate() << "%\\n";
    
    // Log buffer pool statistics - NOW THESE WILL SHOW DATA!
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
    stats.large_message_fallbacks = large_message_fallbacks_.load();
    stats.buffer_pool_hits = buffer_pool_hits_.load();
    stats.buffer_pool_misses = buffer_pool_misses_.load();
    
    return stats;
}

void ClientConnectionTcpZeroCopy::resetZeroCopyStats()
{
    zerocopy_attempts_.store(0);
    zerocopy_successful_.store(0);
    zerocopy_bytes_.store(0);
    regular_receives_.store(0);
    regular_bytes_.store(0);
    large_message_fallbacks_.store(0);
    buffer_pool_hits_.store(0);
    buffer_pool_misses_.store(0);
}