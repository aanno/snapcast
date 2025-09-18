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

// 3rd party headers
#include <boost/asio/ip/tcp.hpp>

// standard headers

using boost::asio::ip::tcp;


/// Abstract base class for TCP-based stream sessions.
/**
 * Abstract TCP session that provides common TCP functionality like socket management,
 * message reading, start/stop lifecycle, and IP address retrieval.
 * Derived classes must implement the sendAsync method with their specific sending strategy.
 */
class StreamSessionTcp : public StreamSession
{
public:
    /// ctor. Received message from the client are passed to StreamMessageReceiver
    StreamSessionTcp(StreamMessageReceiver* receiver, const ServerSettings& server_settings, tcp::socket&& socket);
    ~StreamSessionTcp() override;
    void start() override;
    void stop() override;
    std::string getIP() override;

protected:
    /// Read next message
    void readNext();
    /// Send message @p buffer and pass result to @p handler (pure virtual - must be implemented by derived classes)
    void sendAsync(const std::shared_ptr<shared_const_buffer> buffer, WriteHandler&& handler) override = 0;
    /// The underlying socket
    tcp::socket socket_;
};
