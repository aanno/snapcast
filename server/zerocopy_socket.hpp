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

// 3rd party headers
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>

// standard headers
#include <sys/socket.h>
#include <linux/errqueue.h>
#include <memory>
#include <functional>
#include <vector>
#include <deque>
#include <atomic>
#include <chrono>

using boost::asio::ip::tcp;

/// Handler type for zerocopy completion notifications
using ZeroCopyHandler = std::function<void(boost::system::error_code, std::size_t)>;

/// Buffer tracking structure for zerocopy operations
struct ZeroCopyBuffer
{
    std::shared_ptr<std::vector<char>> data;
    ZeroCopyHandler handler;
    uint32_t sequence_id;
    
    ZeroCopyBuffer(std::shared_ptr<std::vector<char>> buf, ZeroCopyHandler h, uint32_t seq)
        : data(std::move(buf)), handler(std::move(h)), sequence_id(seq) {}
};

/// Boost.Asio socket wrapper with MSG_ZEROCOPY support
class ZeroCopySocket
{
public:
    /// Required for Boost.Asio compatibility
    typedef tcp::socket::executor_type executor_type;
public:
    explicit ZeroCopySocket(boost::asio::io_context& io_context);
    
    /// Move from existing TCP socket
    explicit ZeroCopySocket(tcp::socket&& socket);
    
    ~ZeroCopySocket();

    /// Enable zerocopy on this socket
    bool enableZeroCopy();
    
    /// Check if zerocopy is supported and enabled
    bool isZeroCopyEnabled() const { return zerocopy_enabled_; }
    
    /// Send buffer with zerocopy semantics if enabled, fallback to regular send otherwise
    void async_send_zerocopy(const std::shared_ptr<std::vector<char>>& buffer,
                             ZeroCopyHandler handler);
    
    /// Regular async_write wrapper
    template<typename ConstBufferSequence, typename WriteHandler>
    void async_write_some(const ConstBufferSequence& buffers, WriteHandler&& handler)
    {
        socket_.async_write_some(buffers, std::forward<WriteHandler>(handler));
    }
    
    /// Required for boost::asio::async_read compatibility
    template<typename MutableBufferSequence, typename ReadHandler>
    void async_read_some(const MutableBufferSequence& buffers, ReadHandler&& handler)
    {
        socket_.async_read_some(buffers, std::forward<ReadHandler>(handler));
    }
    
    /// Get remote endpoint
    tcp::endpoint remote_endpoint() const { return socket_.remote_endpoint(); }
    
    /// Get native handle
    tcp::socket::native_handle_type native_handle() { return socket_.native_handle(); }
    
    /// Check if socket is open
    bool is_open() const { return socket_.is_open(); }
    
    /// Shutdown socket
    void shutdown(tcp::socket::shutdown_type what, boost::system::error_code& ec)
    {
        socket_.shutdown(what, ec);
    }
    
    /// Close socket
    void close(boost::system::error_code& ec) { socket_.close(ec); }
    
    /// Get executor
    auto get_executor() { return socket_.get_executor(); }
    
    /// Zerocopy statistics structure
    struct ZeroCopyStats {
        uint64_t zerocopy_sends{0};
        uint64_t regular_sends{0};
        uint64_t zerocopy_bytes{0};
        uint64_t regular_bytes{0};
        uint64_t completions{0};
        uint64_t fallbacks{0};
        std::chrono::steady_clock::time_point start_time;
        
        ZeroCopyStats() : start_time(std::chrono::steady_clock::now()) {}
    };
    
    /// Get zerocopy statistics snapshot
    ZeroCopyStats getStats() const;
    
    /// Print diagnostic information
    void printDiagnostics() const;
    
    /// Reset statistics
    void resetStats();
    
    /// Get IP address for diagnostics
    std::string getIP() const;

private:
    /// Start monitoring error queue for zerocopy completion notifications
    void start_error_queue_monitor();
    
    /// Handle completion notifications from error queue
    void handle_error_queue_notification();
    
    /// Process a single completion notification
    void process_completion_notification(const struct sock_extended_err* err);

    tcp::socket socket_;
    boost::asio::posix::stream_descriptor error_queue_monitor_;
    
    bool zerocopy_enabled_;
    uint32_t next_sequence_id_;
    
    /// Track pending zerocopy buffers
    std::deque<ZeroCopyBuffer> pending_buffers_;
    
    /// Buffer for error queue notifications
    std::vector<char> error_queue_buffer_;
    
    /// Statistics tracking (atomic for thread safety)
    mutable std::atomic<uint64_t> zerocopy_sends_{0};
    mutable std::atomic<uint64_t> regular_sends_{0};
    mutable std::atomic<uint64_t> zerocopy_bytes_{0};
    mutable std::atomic<uint64_t> regular_bytes_{0};
    mutable std::atomic<uint64_t> completions_{0};
    mutable std::atomic<uint64_t> fallbacks_{0};
    mutable std::chrono::steady_clock::time_point start_time_;
};