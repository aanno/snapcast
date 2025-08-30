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

// local headers
#include "stream_session.hpp"
#include "zerocopy_socket.hpp"

// 3rd party headers
#include <boost/asio/ip/tcp.hpp>

// standard headers

using boost::asio::ip::tcp;

/// Zerocopy-enabled endpoint for connected clients
/**
 * Enhanced version of StreamSessionTcp with MSG_ZEROCOPY support
 * Automatically falls back to regular TCP if zerocopy is not available
 */
class StreamSessionTcpZeroCopy : public StreamSession
{
public:
    /// ctor. Received message from the client are passed to StreamMessageReceiver
    StreamSessionTcpZeroCopy(StreamMessageReceiver* receiver, const ServerSettings& server_settings, tcp::socket&& socket);
    ~StreamSessionTcpZeroCopy() override;
    
    void start() override;
    void stop() override;
    std::string getIP() override;

    /// Check if zerocopy is enabled for this session
    bool isZeroCopyEnabled() const { return zerocopy_socket_.isZeroCopyEnabled(); }
    
    /// Print zerocopy diagnostics for this session
    void printZeroCopyDiagnostics() const { zerocopy_socket_.printDiagnostics(); }
    
    /// Get zerocopy statistics for this session
    ZeroCopySocket::ZeroCopyStats getZeroCopyStats() const { return zerocopy_socket_.getStats(); }

protected:
    /// Read next message
    void readNext();
    /// Send message @p buffer with zerocopy optimization when possible
    void sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler) override;

private:
    /// Convert shared_const_buffer to zerocopy buffer format
    std::shared_ptr<std::vector<char>> extractBuffer(const shared_const_buffer& buffer);

    ZeroCopySocket zerocopy_socket_;
};