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
#include "stream_session_tcp_coordinated.hpp"

// local headers
#include "common/aixlog.hpp"

// standard headers
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <cstring>

static constexpr auto LOG_TAG = "StreamSessionTcpCoordinated";

StreamSessionTcpCoordinated::StreamSessionTcpCoordinated(StreamMessageReceiver* receiver, const ServerSettings& server_settings, tcp::socket&& socket)
    : StreamSessionTcp(receiver, server_settings, std::move(socket))
{
    native_socket_ = socket_.native_handle();
    zerocopy_available_ = initializeZeroCopy();
    
    if (zerocopy_available_)
    {
        LOG(INFO, LOG_TAG) << "Coordinated ZeroCopy enabled for session " << getIP();
        error_queue_timer_ = std::make_unique<boost::asio::steady_timer>(socket_.get_executor());
    }
    else
    {
        LOG(INFO, LOG_TAG) << "ZeroCopy not available for session " << getIP() << ", using regular TCP";
    }
}

StreamSessionTcpCoordinated::~StreamSessionTcpCoordinated()
{
    stop();
    
    // Log final statistics
    if (zerocopy_available_)
    {
        auto stats = getZeroCopyStats();
        LOG(INFO, LOG_TAG) << "Session " << getIP() << " final stats - ZC: " << stats.zerocopy_successful 
                           << "/" << stats.zerocopy_attempts << " (" << stats.zerocopy_percentage() << "%), "
                           << "Regular: " << stats.regular_sends << ", Fallbacks: " << stats.coordination_fallbacks;
    }
}

void StreamSessionTcpCoordinated::start()
{
    StreamSessionTcp::start();
    
    if (zerocopy_available_)
    {
        startErrorQueueMonitoring();
    }
}

void StreamSessionTcpCoordinated::stop()
{
    if (zerocopy_available_)
    {
        stopErrorQueueMonitoring();
        
        // Clear any pending sends
        std::lock_guard<std::mutex> lock(pending_sends_mutex_);
        while (!pending_sends_.empty())
        {
            auto& pending = pending_sends_.front();
            if (pending.handler)
            {
                pending.handler(boost::asio::error::operation_aborted, 0);
            }
            pending_sends_.pop();
        }
    }
    
    StreamSessionTcp::stop();
}

bool StreamSessionTcpCoordinated::initializeZeroCopy()
{
    if (native_socket_ < 0)
    {
        LOG(DEBUG, LOG_TAG) << "Invalid native socket handle";
        return false;
    }
    
    // Enable zerocopy on socket
    int enable = 1;
    if (setsockopt(native_socket_, SOL_SOCKET, SO_ZEROCOPY, &enable, sizeof(enable)) < 0)
    {
        LOG(DEBUG, LOG_TAG) << "Failed to enable SO_ZEROCOPY: " << strerror(errno);
        return false;
    }
    
    // Verify zerocopy is enabled
    int enabled = 0;
    socklen_t len = sizeof(enabled);
    if (getsockopt(native_socket_, SOL_SOCKET, SO_ZEROCOPY, &enabled, &len) < 0 || !enabled)
    {
        LOG(DEBUG, LOG_TAG) << "SO_ZEROCOPY verification failed";
        return false;
    }
    
    LOG(DEBUG, LOG_TAG) << "Coordinated ZeroCopy successfully enabled on socket\n";
    return true;
}

bool StreamSessionTcpCoordinated::tryReserveZeroCopy()
{
    // Atomically check if idle and reserve for zerocopy (race-condition safe)
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

void StreamSessionTcpCoordinated::releaseZeroCopy()
{
    // Release zerocopy reservation
    pending_async_operations_.store(0);
}

void StreamSessionTcpCoordinated::sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler)
{
    size_t buffer_size = boost::asio::buffer_size(buffer);
    
    // Decide whether to attempt zerocopy based on size and availability
    bool should_use_zerocopy = zerocopy_available_ && 
                              buffer_size >= ZEROCOPY_THRESHOLD &&
                              tryReserveZeroCopy();
    
    if (should_use_zerocopy)
    {
        LOG(TRACE, LOG_TAG) << "Attempting coordinated zerocopy send for " << buffer_size << " bytes\n";
        sendZeroCopy(buffer, std::move(handler));
    }
    else
    {
        if (zerocopy_available_ && buffer_size >= ZEROCOPY_THRESHOLD)
        {
            coordination_fallbacks_++;
            LOG(DEBUG, LOG_TAG) << "Zerocopy fallback due to pending async ops for " << buffer_size << " bytes";
        }
        sendRegularCoordinated(buffer, std::move(handler));
    }
}

void StreamSessionTcpCoordinated::sendRegularCoordinated(const shared_const_buffer& buffer, WriteHandler&& handler)
{
    // Track the async operation
    pending_async_operations_++;
    regular_sends_++;
    regular_bytes_ += boost::asio::buffer_size(buffer);
    
    // Use the parent class implementation with coordination tracking
    StreamSessionTcp::sendAsync(buffer, [this, handler = std::move(handler)](boost::system::error_code ec, std::size_t bytes_transferred) mutable
    {
        // Decrement pending operations counter
        pending_async_operations_--;
        
        // Call the original handler
        if (handler)
            handler(ec, bytes_transferred);
        
        // Process any queued sends that might now be able to use zerocopy
        processPendingSends();
    });
}

void StreamSessionTcpCoordinated::sendZeroCopy(const shared_const_buffer& buffer, WriteHandler&& handler)
{
    zerocopy_attempts_++;
    
    size_t buffer_size = boost::asio::buffer_size(buffer);
    uint32_t buffer_id = next_buffer_id_++;
    
    // Copy buffer data for zerocopy (kernel needs stable buffer)
    auto zerocopy_buffer = std::make_shared<std::vector<char>>(buffer_size);
    boost::asio::buffer_copy(boost::asio::buffer(*zerocopy_buffer), buffer);
    
    // Prepare message header for sendmsg
    struct msghdr msg = {};
    struct iovec iov = {zerocopy_buffer->data(), zerocopy_buffer->size()};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    
    // Send with MSG_ZEROCOPY
    ssize_t result = sendmsg(native_socket_, &msg, MSG_ZEROCOPY | MSG_DONTWAIT);
    
    if (result < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
        {
            LOG(DEBUG, LOG_TAG) << "ZeroCopy send would block, falling back to regular send";
            releaseZeroCopy(); // Release reservation before fallback
            sendRegularCoordinated(buffer, std::move(handler));
            return;
        }
        else
        {
            LOG(ERROR, LOG_TAG) << "ZeroCopy sendmsg failed: " << strerror(errno);
            releaseZeroCopy(); // Release reservation on error
            if (handler)
                handler(boost::system::error_code(errno, boost::system::system_category()), 0);
            return;
        }
    }
    
    if (static_cast<size_t>(result) != buffer_size)
    {
        LOG(ERROR, LOG_TAG) << "ZeroCopy partial send: " << result << "/" << buffer_size << " bytes";
        releaseZeroCopy(); // Release reservation on partial send error
        if (handler)
            handler(boost::asio::error::message_size, result);
        return;
    }
    
    // Success - zerocopy send completed immediately (synchronously from our perspective)
    zerocopy_successful_++;
    zerocopy_bytes_ += buffer_size;
    
    LOG(TRACE, LOG_TAG) << "ZeroCopy send successful: " << buffer_size << " bytes, ID: " << buffer_id << "\n";
    
    // Release zerocopy reservation
    releaseZeroCopy();
    
    // Complete the handler immediately
    if (handler)
        handler(boost::system::error_code(), buffer_size);
}

void StreamSessionTcpCoordinated::processPendingSends()
{
    // Avoid recursive processing
    if (processing_queue_.exchange(true))
        return;
    
    std::lock_guard<std::mutex> lock(pending_sends_mutex_);
    while (!pending_sends_.empty() && tryReserveZeroCopy())
    {
        auto pending = std::move(pending_sends_.front());
        pending_sends_.pop();
        
        // Process this send now that we can use zerocopy
        if (pending.use_zerocopy)
        {
            sendZeroCopy(pending.buffer, std::move(pending.handler));
        }
        else
        {
            sendRegularCoordinated(pending.buffer, std::move(pending.handler));
        }
    }
    
    processing_queue_.store(false);
}

void StreamSessionTcpCoordinated::startErrorQueueMonitoring()
{
    if (!error_queue_timer_ || monitoring_active_)
        return;
    
    monitoring_active_ = true;
    
    // Start with 10ms polling for error queue
    auto self = shared_from_this();
    error_queue_timer_->expires_after(std::chrono::milliseconds(10));
    error_queue_timer_->async_wait([this, self](boost::system::error_code ec)
    {
        if (!ec && monitoring_active_)
        {
            processErrorQueue();
            
            // Continue monitoring with 10ms interval
            error_queue_timer_->expires_after(std::chrono::milliseconds(10));
            error_queue_timer_->async_wait([this, self](boost::system::error_code ec2)
            {
                if (!ec2 && monitoring_active_)
                {
                    startErrorQueueMonitoring(); // Continue monitoring
                }
            });
        }
    });
}

void StreamSessionTcpCoordinated::stopErrorQueueMonitoring()
{
    monitoring_active_ = false;
    if (error_queue_timer_)
    {
        boost::system::error_code ec;
        error_queue_timer_->cancel(ec);
    }
}

void StreamSessionTcpCoordinated::processErrorQueue()
{
    char control_buf[512];
    struct msghdr msg = {};
    msg.msg_control = control_buf;
    msg.msg_controllen = sizeof(control_buf);
    
    while (true)
    {
        ssize_t ret = recvmsg(native_socket_, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (ret < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                LOG(DEBUG, LOG_TAG) << "Error queue recv failed: " << strerror(errno);
            }
            break; // No more messages or error
        }
        
        // Process control messages
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
        {
            if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR)
            {
                struct sock_extended_err* ee = reinterpret_cast<struct sock_extended_err*>(CMSG_DATA(cmsg));
                if (ee->ee_errno == 0 && ee->ee_origin == SO_EE_ORIGIN_ZEROCOPY)
                {
                    // Zerocopy completion notification
                    uint32_t lo = ee->ee_info;
                    uint32_t hi = ee->ee_data;
                    
                    LOG(DEBUG, LOG_TAG) << "ZeroCopy completion notification: range [" << lo << "-" << hi << "]";
                    // Note: We complete handlers immediately in sendZeroCopy, so we don't need to track pending buffers
                }
            }
        }
    }
}

StreamSessionTcpCoordinated::ZeroCopyStats StreamSessionTcpCoordinated::getZeroCopyStats() const
{
    ZeroCopyStats stats;
    stats.zerocopy_attempts = zerocopy_attempts_.load();
    stats.zerocopy_successful = zerocopy_successful_.load();
    stats.zerocopy_bytes = zerocopy_bytes_.load();
    stats.regular_sends = regular_sends_.load();
    stats.regular_bytes = regular_bytes_.load();
    stats.coordination_fallbacks = coordination_fallbacks_.load();
    
    return stats;
}