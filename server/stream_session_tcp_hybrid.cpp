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
#include "stream_session_tcp_hybrid.hpp"

// local headers
#include "common/aixlog.hpp"

// standard headers
#include <sys/socket.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <cstring>
#include <algorithm>

static constexpr auto LOG_TAG = "StreamSessionTcpHybrid";

StreamSessionTcpHybrid::StreamSessionTcpHybrid(StreamMessageReceiver* receiver, const ServerSettings& server_settings, tcp::socket&& socket)
    : StreamSessionTcp(receiver, server_settings, std::move(socket))
{
    native_socket_ = socket_.native_handle();
    zerocopy_available_ = initializeZeroCopy();
    
    if (zerocopy_available_)
    {
        LOG(INFO, LOG_TAG) << "ZeroCopy enabled for session " << getIP();
        error_queue_timer_ = std::make_unique<boost::asio::steady_timer>(socket_.get_executor());
    }
    else
    {
        LOG(INFO, LOG_TAG) << "ZeroCopy not available for session " << getIP() << ", using regular TCP";
    }
}

StreamSessionTcpHybrid::~StreamSessionTcpHybrid()
{
    stop();
    
    // Log final statistics
    if (zerocopy_available_)
    {
        auto stats = getZeroCopyStats();
        LOG(INFO, LOG_TAG) << "Session " << getIP() << " final stats - ZC: " << stats.zerocopy_successful 
                           << "/" << stats.zerocopy_attempts << " (" << stats.zerocopy_percentage() << "%), "
                           << "Regular: " << stats.regular_sends << ", Pending: " << stats.pending_buffers;
    }
}

void StreamSessionTcpHybrid::start()
{
    StreamSessionTcp::start();
    
    if (zerocopy_available_)
    {
        startErrorQueueMonitoring();
    }
}

void StreamSessionTcpHybrid::stop()
{
    if (zerocopy_available_)
    {
        stopErrorQueueMonitoring();
        
        // Complete any pending zerocopy operations with error
        std::lock_guard<std::mutex> lock(pending_buffers_mutex_);
        for (auto& buffer : pending_buffers_)
        {
            if (buffer.handler)
            {
                buffer.handler(boost::asio::error::operation_aborted, 0);
            }
        }
        pending_buffers_.clear();
    }
    
    StreamSessionTcp::stop();
}

bool StreamSessionTcpHybrid::initializeZeroCopy()
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
    
    LOG(DEBUG, LOG_TAG) << "ZeroCopy successfully enabled on socket";
    return true;
}

void StreamSessionTcpHybrid::sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler)
{
    size_t buffer_size = boost::asio::buffer_size(buffer);
    
    // Use zerocopy selectively for large messages only
    if (zerocopy_available_ && buffer_size >= ZEROCOPY_THRESHOLD)
    {
        // Check if we have too many pending buffers
        std::unique_lock<std::mutex> lock(pending_buffers_mutex_);
        if (pending_buffers_.size() >= MAX_PENDING_BUFFERS)
        {
            lock.unlock();
            LOG(DEBUG, LOG_TAG) << "Too many pending zerocopy buffers (" << pending_buffers_.size() 
                               << "), falling back to regular send for " << buffer_size << " bytes";
            sendRegular(buffer, std::move(handler));
            return;
        }
        lock.unlock();
        
        LOG(DEBUG, LOG_TAG) << "Attempting zerocopy send for " << buffer_size << " bytes";
        sendZeroCopy(buffer, std::move(handler));
    }
    else
    {
        // Use regular TCP for small messages or when zerocopy unavailable
        sendRegular(buffer, std::move(handler));
    }
}

void StreamSessionTcpHybrid::sendRegular(const shared_const_buffer& buffer, WriteHandler&& handler)
{
    regular_sends_++;
    regular_bytes_ += boost::asio::buffer_size(buffer);
    
    // Delegate to parent class implementation
    StreamSessionTcp::sendAsync(buffer, std::move(handler));
}

void StreamSessionTcpHybrid::sendZeroCopy(const shared_const_buffer& buffer, WriteHandler&& handler)
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
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            LOG(DEBUG, LOG_TAG) << "ZeroCopy send would block, falling back to regular send";
            sendRegular(buffer, std::move(handler));
            return;
        }
        else if (errno == ENOBUFS)
        {
            LOG(DEBUG, LOG_TAG) << "ZeroCopy send buffer full, falling back to regular send";
            sendRegular(buffer, std::move(handler));
            return;
        }
        else
        {
            LOG(ERROR, LOG_TAG) << "ZeroCopy sendmsg failed: " << strerror(errno);
            if (handler)
                handler(boost::system::error_code(errno, boost::system::system_category()), 0);
            return;
        }
    }
    
    if (static_cast<size_t>(result) != buffer_size)
    {
        LOG(ERROR, LOG_TAG) << "ZeroCopy partial send: " << result << "/" << buffer_size << " bytes";
        if (handler)
            handler(boost::asio::error::message_size, result);
        return;
    }
    
    // Success - track the buffer for completion notification
    zerocopy_successful_++;
    zerocopy_bytes_ += buffer_size;
    
    {
        std::lock_guard<std::mutex> lock(pending_buffers_mutex_);
        pending_buffers_.push_back({zerocopy_buffer, std::move(handler), buffer_id, buffer_size});
    }
    
    LOG(DEBUG, LOG_TAG) << "ZeroCopy send successful: " << buffer_size << " bytes, ID: " << buffer_id;
}

void StreamSessionTcpHybrid::startErrorQueueMonitoring()
{
    if (!error_queue_timer_ || monitoring_active_)
        return;
    
    monitoring_active_ = true;
    
    // Start with 1ms polling
    auto self = shared_from_this();
    error_queue_timer_->expires_after(std::chrono::milliseconds(1));
    error_queue_timer_->async_wait([this, self](boost::system::error_code ec)
    {
        if (!ec && monitoring_active_)
        {
            processErrorQueue();
            
            // Adaptive polling interval based on pending buffer count
            std::unique_lock<std::mutex> lock(pending_buffers_mutex_);
            size_t pending_count = pending_buffers_.size();
            lock.unlock();
            
            std::chrono::milliseconds interval;
            if (pending_count > 32)
                interval = std::chrono::milliseconds(1);    // Aggressive when many pending
            else if (pending_count > 8)
                interval = std::chrono::milliseconds(5);    // Medium when some pending
            else if (pending_count > 0)
                interval = std::chrono::milliseconds(10);   // Relaxed when few pending
            else
                interval = std::chrono::milliseconds(50);   // Very relaxed when none pending
            
            error_queue_timer_->expires_after(interval);
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

void StreamSessionTcpHybrid::stopErrorQueueMonitoring()
{
    monitoring_active_ = false;
    if (error_queue_timer_)
    {
        boost::system::error_code ec;
        error_queue_timer_->cancel(ec);
    }
}

void StreamSessionTcpHybrid::processErrorQueue()
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
                    
                    completions_received_++;
                    
                    // Complete buffers in range [lo, hi]
                    std::unique_lock<std::mutex> lock(pending_buffers_mutex_);
                    auto it = pending_buffers_.begin();
                    while (it != pending_buffers_.end())
                    {
                        if (it->id >= lo && it->id <= hi)
                        {
                            // Complete this buffer
                            if (it->handler)
                            {
                                it->handler(boost::system::error_code(), it->size);
                            }
                            
                            LOG(DEBUG, LOG_TAG) << "ZeroCopy buffer completed: ID " << it->id 
                                               << ", size " << it->size << " bytes";
                            
                            it = pending_buffers_.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                    lock.unlock();
                    
                    LOG(DEBUG, LOG_TAG) << "ZeroCopy completion: range [" << lo << "-" << hi 
                                       << "], remaining pending: " << pending_buffers_.size();
                }
            }
        }
    }
}

StreamSessionTcpHybrid::ZeroCopyStats StreamSessionTcpHybrid::getZeroCopyStats() const
{
    ZeroCopyStats stats;
    stats.zerocopy_attempts = zerocopy_attempts_.load();
    stats.zerocopy_successful = zerocopy_successful_.load();
    stats.zerocopy_bytes = zerocopy_bytes_.load();
    stats.regular_sends = regular_sends_.load();
    stats.regular_bytes = regular_bytes_.load();
    stats.completions_received = completions_received_.load();
    
    {
        std::lock_guard<std::mutex> lock(pending_buffers_mutex_);
        stats.pending_buffers = pending_buffers_.size();
    }
    
    return stats;
}