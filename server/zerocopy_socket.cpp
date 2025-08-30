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

#include "zerocopy_socket.hpp"
#include "common/aixlog.hpp"

// standard headers
#include <iomanip>
#include <sstream>
#include <algorithm>

// system headers
#include <sys/socket.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <unistd.h>
#include <errno.h>

static constexpr auto LOG_TAG = "ZeroCopySocket";

ZeroCopySocket::ZeroCopySocket(boost::asio::io_context& io_context)
    : socket_(io_context), error_queue_timer_(io_context), 
      zerocopy_enabled_(false), next_sequence_id_(1), error_queue_buffer_(1024),
      start_time_(std::chrono::steady_clock::now())
{
}

ZeroCopySocket::ZeroCopySocket(tcp::socket&& socket)
    : socket_(std::move(socket)), error_queue_timer_(socket_.get_executor()),
      zerocopy_enabled_(false), next_sequence_id_(1), error_queue_buffer_(1024),
      start_time_(std::chrono::steady_clock::now())
{
    enableZeroCopy();
}

ZeroCopySocket::~ZeroCopySocket()
{
    boost::system::error_code ec;
    error_queue_timer_.cancel(ec);
}

bool ZeroCopySocket::enableZeroCopy()
{
    if (!socket_.is_open())
    {
        LOG(WARNING, LOG_TAG) << "Cannot enable zerocopy on closed socket\n";
        return false;
    }

    // Enable SO_ZEROCOPY socket option
    int enable = 1;
    int result = setsockopt(socket_.native_handle(), SOL_SOCKET, SO_ZEROCOPY, &enable, sizeof(enable));
    
    if (result != 0)
    {
        LOG(WARNING, LOG_TAG) << "Failed to enable SO_ZEROCOPY: " << strerror(errno) << "\n";
        zerocopy_enabled_ = false;
        return false;
    }

    zerocopy_enabled_ = true;
    
    // Start error queue monitoring (will be triggered by zerocopy operations)
    LOG(INFO, LOG_TAG) << "MSG_ZEROCOPY enabled successfully\n";
    start_error_queue_monitor();

    return true;
}

void ZeroCopySocket::async_send_zerocopy(const std::shared_ptr<std::vector<char>>& buffer,
                                         ZeroCopyHandler handler)
{
    // Simplified approach: Only use zerocopy for large buffers (>4KB audio chunks)
    // Keep small control messages on regular TCP to avoid timing issues
    if (!zerocopy_enabled_ || buffer->empty() || buffer->size() < 4096)
    {
        // Use regular async_write for small messages and when zerocopy disabled
        regular_sends_.fetch_add(1);
        regular_bytes_.fetch_add(buffer->size());
        boost::asio::async_write(socket_, boost::asio::buffer(*buffer),
            [handler = std::move(handler)](boost::system::error_code ec, std::size_t bytes_sent)
            {
                if (handler)
                    handler(ec, bytes_sent);
            });
        return;
    }

    // For large buffers, try immediate zerocopy (no waiting)
    try_immediate_zerocopy_send(buffer, std::move(handler));
}

void ZeroCopySocket::try_immediate_zerocopy_send(const std::shared_ptr<std::vector<char>>& buffer, ZeroCopyHandler handler)
{
    // Prepare iovec for sendmsg
    struct iovec iov{};
    iov.iov_base = buffer->data();
    iov.iov_len = buffer->size();

    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    // Attempt immediate zerocopy send with MSG_DONTWAIT
    // Since socket is writable, this should succeed or immediately return EAGAIN
    ssize_t ret = sendmsg(socket_.native_handle(), &msg, MSG_ZEROCOPY | MSG_DONTWAIT);

    if (ret >= 0)
    {
        // Success! Track buffer for completion notification
        uint32_t seq_id = next_sequence_id_++;
        pending_buffers_.emplace_back(buffer, std::move(handler), seq_id);
        
        // Update statistics
        zerocopy_sends_.fetch_add(1);
        zerocopy_bytes_.fetch_add(buffer->size());
        
        LOG(DEBUG, LOG_TAG) << "Immediate zerocopy send successful: " << ret << " bytes, sequence: " << seq_id << "\n";
        return;
    }

    // Failed - fallback to regular async_write
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
    {
        // Expected: socket not immediately writable or no zerocopy buffers available
        fallbacks_.fetch_add(1);
        LOG(DEBUG, LOG_TAG) << "Zerocopy not available (" << strerror(errno) << "), falling back to regular send\n";
    }
    else
    {
        LOG(WARNING, LOG_TAG) << "Zerocopy sendmsg failed: " << strerror(errno) << ", falling back to regular send\n";
    }

    // Fallback to regular async_write
    regular_sends_.fetch_add(1);
    regular_bytes_.fetch_add(buffer->size());
    boost::asio::async_write(socket_, boost::asio::buffer(*buffer),
        [handler = std::move(handler)](boost::system::error_code ec, std::size_t bytes_sent)
        {
            if (handler)
                handler(ec, bytes_sent);
        });
}

void ZeroCopySocket::perform_zerocopy_send(const std::shared_ptr<std::vector<char>>& buffer, uint32_t seq_id)
{
    // Prepare iovec for sendmsg
    struct iovec iov{};
    iov.iov_base = buffer->data();
    iov.iov_len = buffer->size();

    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    // Use sendmsg syscall directly with MSG_ZEROCOPY but WITHOUT MSG_DONTWAIT
    // This ensures proper ordering since we're called from the io_context
    ssize_t ret = sendmsg(socket_.native_handle(), &msg, MSG_ZEROCOPY);

    if (ret == -1)
    {
        boost::system::error_code ec(errno, boost::system::generic_category());
        
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Socket would block, fallback to regular async_write
            fallbacks_.fetch_add(1);
            regular_sends_.fetch_add(1);
            regular_bytes_.fetch_add(buffer->size());
            LOG(DEBUG, LOG_TAG) << "Socket would block, falling back to regular send\n";
            
            // Find and call the handler for this sequence ID
            auto it = std::find_if(pending_buffers_.begin(), pending_buffers_.end(),
                [seq_id](const ZeroCopyBuffer& buf) { return buf.sequence_id == seq_id; });
            
            if (it != pending_buffers_.end())
            {
                auto handler = std::move(it->handler);
                pending_buffers_.erase(it);
                
                boost::asio::async_write(socket_, boost::asio::buffer(*buffer),
                    [handler = std::move(handler)](boost::system::error_code ec, std::size_t bytes_sent)
                    {
                        if (handler)
                            handler(ec, bytes_sent);
                    });
            }
        }
        else
        {
            LOG(ERROR, LOG_TAG) << "sendmsg with MSG_ZEROCOPY failed: " << strerror(errno) << "\n";
            
            // Find and call the handler with error
            auto it = std::find_if(pending_buffers_.begin(), pending_buffers_.end(),
                [seq_id](const ZeroCopyBuffer& buf) { return buf.sequence_id == seq_id; });
            
            if (it != pending_buffers_.end())
            {
                auto handler = std::move(it->handler);
                pending_buffers_.erase(it);
                if (handler)
                    handler(ec, 0);
            }
        }
        return;
    }

    // Update zerocopy statistics
    zerocopy_sends_.fetch_add(1);
    zerocopy_bytes_.fetch_add(buffer->size());
    
    LOG(DEBUG, LOG_TAG) << "Zerocopy send queued: " << ret << " bytes, sequence: " << seq_id << "\n";
    
    // Note: The handler will be called when we receive the completion notification
    // via the error queue monitoring system
}

void ZeroCopySocket::start_error_queue_monitor()
{
    if (!zerocopy_enabled_)
        return;

    handle_error_queue_notification();
}

void ZeroCopySocket::handle_error_queue_notification()
{
    // Check for pending error queue messages with non-blocking read
    char control_buffer[1024];
    struct iovec iov{};
    iov.iov_base = error_queue_buffer_.data();
    iov.iov_len = error_queue_buffer_.size();

    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control_buffer;
    msg.msg_controllen = sizeof(control_buffer);

    ssize_t ret = recvmsg(socket_.native_handle(), &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
    
    if (ret >= 0)
    {
        // Parse control messages for zerocopy completions
        struct cmsghdr* cmsg;
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg))
        {
            if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR)
            {
                struct sock_extended_err* err = 
                    reinterpret_cast<struct sock_extended_err*>(CMSG_DATA(cmsg));
                
                if (err->ee_origin == SO_EE_ORIGIN_ZEROCOPY)
                {
                    process_completion_notification(err);
                }
            }
        }
        
        // If we got a message, check immediately for more
        error_queue_timer_.expires_after(std::chrono::milliseconds(1));
    }
    else if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        // No messages available, check again in 10ms
        error_queue_timer_.expires_after(std::chrono::milliseconds(10));
    }
    else
    {
        LOG(WARNING, LOG_TAG) << "recvmsg error queue failed: " << strerror(errno) << "\n";
        error_queue_timer_.expires_after(std::chrono::milliseconds(100));
    }

    // Schedule next check
    error_queue_timer_.async_wait([this](boost::system::error_code ec)
    {
        if (ec != boost::asio::error::operation_aborted)
            handle_error_queue_notification();
    });
}

void ZeroCopySocket::process_completion_notification(const struct sock_extended_err* err)
{
    uint32_t lo = err->ee_info;
    uint32_t hi = err->ee_data;
    
    LOG(DEBUG, LOG_TAG) << "Zerocopy completion: range " << lo << "-" << hi << "\n";

    // Find and complete matching buffers
    auto it = pending_buffers_.begin();
    while (it != pending_buffers_.end())
    {
        if (it->sequence_id >= lo && it->sequence_id <= hi)
        {
            // This buffer completed
            if (it->handler)
            {
                boost::system::error_code ec;
                it->handler(ec, it->data->size());
            }
            it = pending_buffers_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    
    // Update completion statistics
    completions_.fetch_add(hi - lo + 1);
}

ZeroCopySocket::ZeroCopyStats ZeroCopySocket::getStats() const
{
    ZeroCopyStats stats;
    stats.zerocopy_sends = zerocopy_sends_.load();
    stats.regular_sends = regular_sends_.load();
    stats.zerocopy_bytes = zerocopy_bytes_.load();
    stats.regular_bytes = regular_bytes_.load();
    stats.completions = completions_.load();
    stats.fallbacks = fallbacks_.load();
    stats.start_time = start_time_;
    return stats;
}

void ZeroCopySocket::printDiagnostics() const
{
    auto stats = getStats();
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - stats.start_time).count();
    
    std::ostringstream oss;
    oss << "ZeroCopy Socket Diagnostics for " << getIP() << ":\n";
    oss << "  Runtime: " << duration << "s\n";
    oss << "  Zerocopy enabled: " << (zerocopy_enabled_ ? "YES" : "NO") << "\n";
    oss << "  Zerocopy sends: " << stats.zerocopy_sends << " (" << stats.zerocopy_bytes << " bytes)\n";
    oss << "  Regular sends: " << stats.regular_sends << " (" << stats.regular_bytes << " bytes)\n";
    oss << "  Completions: " << stats.completions << "\n";
    oss << "  Fallbacks: " << stats.fallbacks << "\n";
    oss << "  Pending buffers: " << pending_buffers_.size() << "\n";
    
    if (stats.zerocopy_sends > 0 || stats.regular_sends > 0) {
        uint64_t total_sends = stats.zerocopy_sends + stats.regular_sends;
        uint64_t total_bytes = stats.zerocopy_bytes + stats.regular_bytes;
        double zerocopy_ratio = (double)stats.zerocopy_sends / total_sends * 100.0;
        double zerocopy_byte_ratio = (double)stats.zerocopy_bytes / total_bytes * 100.0;
        oss << "  Zerocopy ratio: " << std::fixed << std::setprecision(1) << zerocopy_ratio << "% (by count), " 
            << zerocopy_byte_ratio << "% (by bytes)\n";
    }
    
    LOG(INFO, LOG_TAG) << oss.str();
}

void ZeroCopySocket::resetStats()
{
    zerocopy_sends_ = 0;
    regular_sends_ = 0;
    zerocopy_bytes_ = 0;
    regular_bytes_ = 0;
    completions_ = 0;
    fallbacks_ = 0;
    start_time_ = std::chrono::steady_clock::now();
}

std::string ZeroCopySocket::getIP() const
{
    try
    {
        return socket_.remote_endpoint().address().to_string();
    }
    catch (...)
    {
        return "unknown";
    }
}