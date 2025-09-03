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
#include <librist/librist.h>

// standard headers
#include <memory>
#include <atomic>
#include <thread>

/// RIST streaming endpoint for a connected client.
/**
 * RIST endpoint for a connected client.
 * Messages are sent to the client with the "send" method using RIST protocol.
 * Received messages from the client are passed to the StreamMessageReceiver callback
 */
class StreamSessionRist : public StreamSession
{
public:
    /// ctor. Received message from the client are passed to StreamMessageReceiver
    StreamSessionRist(StreamMessageReceiver* receiver, const ServerSettings& server_settings, 
                      const std::string& client_address, uint16_t client_port,
                      boost::asio::io_context& io_context);
    ~StreamSessionRist() override;
    void start() override;
    void stop() override;
    std::string getIP() override;

protected:
    /// Send message @p buffer using RIST and pass result to @p handler
    void sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler) override;

private:
    /// Initialize RIST sender context
    bool initRist();
    /// Cleanup RIST context
    void cleanupRist();
    /// Read messages from RIST (placeholder for bidirectional communication)
    void readNext();
    /// RIST connection status callback
    static void connectionStatusCallback(void* arg, struct rist_peer* peer, enum rist_connection_status status);

private:
    struct rist_ctx* rist_ctx_{nullptr};     ///< RIST context
    struct rist_peer* rist_peer_{nullptr};   ///< RIST peer
    std::string client_address_;             ///< client IP address
    uint16_t client_port_;                   ///< client port
    std::atomic<bool> connected_{false};     ///< connection status
    std::atomic<bool> running_{false};       ///< running status
    std::thread read_thread_;                ///< thread for reading (placeholder)
};