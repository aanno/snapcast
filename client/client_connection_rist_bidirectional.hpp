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

#ifdef HAS_LIBRIST

// local headers
#include "client_connection.hpp"
#include "client_settings.hpp"
#include "common/rist_transport.hpp"

// 3rd party headers
#include <boost/asio/ip/tcp.hpp>

// standard headers
#include <memory>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>

/// Simple RIST client connection using RistTransport
/**
 * RIST client connection using the clean RistTransport class.
 * Follows the same parallel transport pattern as the server.
 * Uses virtual ports for communication:
 * - Audio data: virtual port 1000 (received)
 * - Control messages: virtual port 2000 (received) 
 * - Backchannel: virtual port 3000 (sent)
 */
class ClientConnectionRistBidirectional : public ClientConnection, public RistTransportReceiver
{
public:
    /// c'tor
    ClientConnectionRistBidirectional(boost::asio::io_context& io_context, ClientSettings::Server server);
    /// d'tor
    virtual ~ClientConnectionRistBidirectional();

    void disconnect() override;
    std::string getMacAddress() override;
    void getNextMessage(const MessageHandler<msg::BaseMessage>& handler) override;

    // RistTransportReceiver interface
    void onRistMessageReceived(const msg::BaseMessage& baseMessage, const std::string& payload, uint16_t vport) override;
    void onRistClientConnected(const std::string& clientId) override;
    void onRistClientDisconnected(const std::string& clientId) override;

private:
    boost::system::error_code doConnect(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint) override;
    void write(boost::asio::streambuf& buffer, WriteHandler&& write_handler) override;

    /// Send Hello message to initiate handshake
    void sendHello();
    /// Message processing thread
    void messageProcessorThread();

private:
    std::unique_ptr<RistTransport> rist_transport_;        ///< RIST transport instance
    
    std::queue<std::unique_ptr<msg::BaseMessage>> message_queue_; ///< Message queue
    std::mutex queue_mutex_;                               ///< Protect message queue
    std::condition_variable queue_cv_;                     ///< Notify message thread
    std::thread message_thread_;                           ///< Message processing thread
    
    MessageHandler<msg::BaseMessage> pending_handler_;     ///< Pending message handler
    std::mutex handler_mutex_;                             ///< Protect handler
    
    std::atomic<bool> running_;                            ///< Thread running flag
};

#endif // HAS_LIBRIST