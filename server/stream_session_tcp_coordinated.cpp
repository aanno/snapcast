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

// MSG_ZEROCOPY definition for compatibility with older headers
#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

static constexpr auto LOG_TAG = "StreamSessionTcpCoordinated";

// Global buffer registry for multi-client reference counting
std::map<uint32_t, std::shared_ptr<StreamSessionTcpCoordinated::GlobalBufferRef>> StreamSessionTcpCoordinated::global_buffer_registry_;
std::mutex StreamSessionTcpCoordinated::global_buffer_mutex_;

StreamSessionTcpCoordinated::StreamSessionTcpCoordinated(StreamMessageReceiver* receiver, const ServerSettings& server_settings, tcp::socket&& socket)
    : StreamSessionTcp(receiver, server_settings, std::move(socket))
{
    native_socket_ = socket_.native_handle();
    LOG(DEBUG, LOG_TAG) << "Native socket handle: " << native_socket_ << "\n";
    zerocopy_available_ = initializeZeroCopy();
    
    if (zerocopy_available_)
    {
        LOG(INFO, LOG_TAG) << "Coordinated ZeroCopy enabled for session " << getIP() << "\n";
        error_queue_timer_ = std::make_unique<boost::asio::steady_timer>(socket_.get_executor());
    }
    else
    {
        LOG(INFO, LOG_TAG) << "ZeroCopy not available for session " << getIP() << ", using regular TCP\n";
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
                           << "Regular: " << stats.regular_sends << ", Fallbacks: " << stats.coordination_fallbacks << "\n";
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
    LOG(DEBUG, LOG_TAG) << "initializeZeroCopy called with native_socket_: " << native_socket_ << "\n";
    if (native_socket_ < 0)
    {
        LOG(DEBUG, LOG_TAG) << "Invalid native socket handle: " << native_socket_ << "\n";
        return false;
    }
    
    // Enable zerocopy on socket
    int enable = 1;
    if (setsockopt(native_socket_, SOL_SOCKET, SO_ZEROCOPY, &enable, sizeof(enable)) < 0)
    {
        LOG(DEBUG, LOG_TAG) << "Failed to enable SO_ZEROCOPY: " << strerror(errno) << "\n";
        return false;
    }
    
    // Verify zerocopy is enabled
    int enabled = 0;
    socklen_t len = sizeof(enabled);
    if (getsockopt(native_socket_, SOL_SOCKET, SO_ZEROCOPY, &enabled, &len) < 0 || !enabled)
    {
        LOG(DEBUG, LOG_TAG) << "SO_ZEROCOPY verification failed\n";
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
    pending_async_operations_--;
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
            LOG(DEBUG, LOG_TAG) << "Zerocopy fallback due to pending async ops for " << buffer_size << " bytes\n";
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
    
    // Generate simple sequential buffer ID that matches kernel MSG_ZEROCOPY numbering
    // MSG_ZEROCOPY assigns IDs sequentially per socket, we must match this scheme
    uint32_t buffer_id = next_buffer_id_++;
    
    // Get or create shared buffer for multi-client zerocopy
    std::shared_ptr<GlobalBufferRef> global_buffer_ref;
    std::shared_ptr<std::vector<char>> zerocopy_buffer;
    
    {
        std::lock_guard<std::mutex> lock(global_buffer_mutex_);
        auto it = global_buffer_registry_.find(buffer_id);
        if (it != global_buffer_registry_.end()) {
            // Buffer already exists, increment reference count
            global_buffer_ref = it->second;
            global_buffer_ref->ref_count++;
            zerocopy_buffer = global_buffer_ref->buffer;
            buffer_reuse_count_++;
            LOG(TRACE, LOG_TAG) << "Reusing shared zerocopy buffer ID " << buffer_id << ", ref_count: " << global_buffer_ref->ref_count.load() << "\n";
        } else {
            // Create new shared buffer
            zerocopy_buffer = std::make_shared<std::vector<char>>(buffer_size);
            boost::asio::buffer_copy(boost::asio::buffer(*zerocopy_buffer), buffer);
            
            global_buffer_ref = std::make_shared<GlobalBufferRef>();
            global_buffer_ref->buffer = zerocopy_buffer;
            global_buffer_ref->ref_count = 1;
            global_buffer_ref->create_time = std::chrono::steady_clock::now();
            
            global_buffer_registry_[buffer_id] = global_buffer_ref;
            LOG(TRACE, LOG_TAG) << "Created new shared zerocopy buffer ID " << buffer_id << ", size: " << buffer_size << "\n";
        }
    }
    
    // Prepare message header for sendmsg
    struct msghdr msg = {};
    struct iovec iov = {zerocopy_buffer->data(), zerocopy_buffer->size()};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    
    // Send with MSG_ZEROCOPY
    // LOG(DEBUG, LOG_TAG) << "Attempting sendmsg with MSG_ZEROCOPY|MSG_DONTWAIT, buffer_size: " << buffer_size << "\n";
    ssize_t result = sendmsg(native_socket_, &msg, MSG_ZEROCOPY | MSG_DONTWAIT);
    LOG(TRACE, LOG_TAG) << "sendmsg result: " << result << ", errno: " << (result < 0 ? strerror(errno) : "success") << "\n";
    
    if (result < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
        {
            LOG(DEBUG, LOG_TAG) << "ZeroCopy send would block, falling back to regular send\n";
            releaseZeroCopy(); // Release reservation before fallback
            sendRegularCoordinated(buffer, std::move(handler));
            return;
        }
        else
        {
            LOG(ERROR, LOG_TAG) << "ZeroCopy sendmsg failed: " << strerror(errno) << "\n";
            releaseZeroCopy(); // Release reservation on error
            if (handler)
                handler(boost::system::error_code(errno, boost::system::system_category()), 0);
            return;
        }
    }
    
    if (static_cast<size_t>(result) != buffer_size)
    {
        LOG(ERROR, LOG_TAG) << "ZeroCopy partial send: " << result << "/" << buffer_size << " bytes\n";
        releaseZeroCopy(); // Release reservation on partial send error
        if (handler)
            handler(boost::asio::error::message_size, result);
        return;
    }
    
    // Success - zerocopy send completed immediately (synchronously from our perspective)
    zerocopy_successful_++;
    zerocopy_bytes_ += buffer_size;
    outstanding_zerocopy_buffers_++;
    
    // Track the buffer for completion notification
    {
        std::lock_guard<std::mutex> lock(zerocopy_buffers_mutex_);
        pending_zerocopy_buffers_[buffer_id] = {
            zerocopy_buffer,
            buffer_id,
            std::chrono::steady_clock::now()
        };
    }
    
    LOG(TRACE, LOG_TAG) << "ZeroCopy send successful: " << buffer_size << " bytes, ID: " << buffer_id << ", tracking for completion\n";
    
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
    LOG(DEBUG, LOG_TAG) << "Starting error queue monitoring for session " << getIP() << "\n";
    
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
                else
                {
                    LOG(DEBUG, LOG_TAG) << "Error queue monitoring timer cancelled or error: " << ec2.message() << "\n";
                    // startErrorQueueMonitoring(); // Continue monitoring even on error
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
    static int call_count = 0;
    static int debug_call_count = 0;
    if (++call_count % 100 == 1) { // Log every 100th call to avoid spam
        LOG(TRACE, LOG_TAG) << "Processing error queue (call #" << call_count << ")\n";
    }
    if (++debug_call_count % 1000 == 1) { // Log every 1000th call for proof it's running
        LOG(DEBUG, LOG_TAG) << "processErrorQueue() is active (call #" << debug_call_count << "), pending buffers: " << pending_zerocopy_buffers_.size() << "\n";
    }
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
                LOG(DEBUG, LOG_TAG) << "Error queue recv failed: " << strerror(errno) << "\n";
            }
            break; // No more messages or error
        }
        
        LOG(TRACE, LOG_TAG) << "Received error queue message, size: " << ret << "\n";
        
        // Process control messages
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
        {
            LOG(TRACE, LOG_TAG) << "Control message: level=" << cmsg->cmsg_level << ", type=" << cmsg->cmsg_type << "\n";
            if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR)
            {
                struct sock_extended_err* ee = reinterpret_cast<struct sock_extended_err*>(CMSG_DATA(cmsg));
                if (ee->ee_errno == 0 && ee->ee_origin == SO_EE_ORIGIN_ZEROCOPY)
                {
                    // Zerocopy completion notification
                    uint32_t lo = ee->ee_info;
                    uint32_t hi = ee->ee_data;
                    
                    LOG(DEBUG, LOG_TAG) << "ZeroCopy completion notification: range [" << lo << "-" << hi << "], tracking " << pending_zerocopy_buffers_.size() << " buffers\n";
                    completion_notifications_received_++;
                    
                    // Release buffers in the completed range with reference counting
                    {
                        std::lock_guard<std::mutex> session_lock(zerocopy_buffers_mutex_);
                        for (uint32_t buffer_id = lo; buffer_id <= hi; ++buffer_id) {
                            auto session_it = pending_zerocopy_buffers_.find(buffer_id);
                            if (session_it != pending_zerocopy_buffers_.end()) {
                                auto duration = std::chrono::steady_clock::now() - session_it->second.send_time;
                                auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
                                
                                // Decrement global reference count
                                {
                                    std::lock_guard<std::mutex> global_lock(global_buffer_mutex_);
                                    auto global_it = global_buffer_registry_.find(buffer_id);
                                    if (global_it != global_buffer_registry_.end()) {
                                        auto ref_count = --global_it->second->ref_count;
                                        LOG(DEBUG, LOG_TAG) << "Completed zerocopy buffer ID " << buffer_id << " after " << duration_ms << "ms, remaining refs: " << ref_count << "\n";
                                        
                                        if (ref_count <= 0) {
                                            // Last reference - can release global buffer
                                            auto total_duration = std::chrono::steady_clock::now() - global_it->second->create_time;
                                            auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_duration).count();
                                            LOG(DEBUG, LOG_TAG) << "Releasing global zerocopy buffer ID " << buffer_id << " after " << total_ms << "ms total lifetime\n";
                                            global_buffer_registry_.erase(global_it);
                                        }
                                    } else {
                                        LOG(WARNING, LOG_TAG) << "Completion notification for buffer ID " << buffer_id << " not found in global registry\n";
                                    }
                                }
                                
                                pending_zerocopy_buffers_.erase(session_it);
                                outstanding_zerocopy_buffers_--;
                            } else {
                                LOG(WARNING, LOG_TAG) << "Completion notification for unknown session buffer ID " << buffer_id << "\n";
                            }
                        }
                    }
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
    stats.pending_async_operations = pending_async_operations_.load();
    stats.outstanding_zerocopy_buffers = outstanding_zerocopy_buffers_.load();
    stats.completion_notifications_received = completion_notifications_received_.load();
    stats.completion_notifications_missing = completion_notifications_missing_.load();
    stats.buffer_reuse_count = buffer_reuse_count_.load();
    
    // Cleanup stale buffers and get global shared buffer count
    StreamSessionTcpCoordinated::cleanupStaleBuffers();
    {
        std::lock_guard<std::mutex> lock(global_buffer_mutex_);
        stats.global_shared_buffers = global_buffer_registry_.size();
    }
    
    return stats;
}

void StreamSessionTcpCoordinated::resetZeroCopyStats()
{
    zerocopy_attempts_.store(0);
    zerocopy_successful_.store(0);
    zerocopy_bytes_.store(0);
    regular_sends_.store(0);
    regular_bytes_.store(0);
    coordination_fallbacks_.store(0);
    completion_notifications_received_.store(0);
    completion_notifications_missing_.store(0);
    buffer_reuse_count_.store(0);
    // Note: outstanding_zerocopy_buffers, global_shared_buffers and pending_async_operations are not reset as they represent current state
}

void StreamSessionTcpCoordinated::cleanupStaleBuffers()
{
    std::lock_guard<std::mutex> lock(global_buffer_mutex_);
    auto now = std::chrono::steady_clock::now();
    
    auto it = global_buffer_registry_.begin();
    size_t cleaned_count = 0;
    while (it != global_buffer_registry_.end()) {
        if (now - it->second->create_time > BUFFER_TIMEOUT) {
            LOG(TRACE, LOG_TAG) << "Timeout cleanup of stale buffer ID " << it->first << " after " 
                                  << std::chrono::duration_cast<std::chrono::seconds>(now - it->second->create_time).count() << "s\n";
            cleaned_count++;
            it = global_buffer_registry_.erase(it);
        } else {
            ++it;
        }
    }
    
    if (cleaned_count > 0) {
        LOG(DEBUG, LOG_TAG) << "Cleaned up " << cleaned_count << " stale zerocopy buffers due to missing completion notifications\n";
    }
}
