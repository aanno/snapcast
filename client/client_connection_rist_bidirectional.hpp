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

// 3rd party headers
#include <librist/librist.h>
#include <boost/asio/ip/tcp.hpp>

// standard headers
#include <atomic>
#include <memory>
#include <thread>
#include <mutex>
#include <vector>

/// Bidirectional RIST client connection
/**
 * Bidirectional RIST client connection endpoint using multiplexing.
 * Uses virtual ports to separate different data types:
 * - Audio data: virtual port 1000 (received)
 * - Control messages: virtual port 2000 (received)
 * - Backchannel: virtual port 3000 (sent)
 * Receives media data and control messages via RIST protocol.
 * Sends backchannel messages via RIST protocol.
 */
class ClientConnectionRistBidirectional : public ClientConnection
{
public:
    /// c'tor
    ClientConnectionRistBidirectional(boost::asio::io_context& io_context, ClientSettings::Server server);
    /// d'tor
    virtual ~ClientConnectionRistBidirectional();

    void disconnect() override;
    std::string getMacAddress() override;
    void getNextMessage(const MessageHandler<msg::BaseMessage>& handler) override;

private:
    boost::system::error_code doConnect(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint) override;
    void write(boost::asio::streambuf& buffer, WriteHandler&& write_handler) override;

    /// Initialize RIST receiver and sender
    bool initRist();
    /// Cleanup RIST contexts
    void cleanupRist();
    /// RIST receiver thread for audio/control messages
    void ristReceiverThread();
    /// RIST connection status callbacks
    static void receiverConnectionStatusCallback(void* arg, struct rist_peer* peer, enum rist_connection_status status);
    static void senderConnectionStatusCallback(void* arg, struct rist_peer* peer, enum rist_connection_status status);

private:
    // Virtual ports for multiplexing (must match server)
    static constexpr uint16_t VPORT_AUDIO = 1000;        ///< Audio data port
    static constexpr uint16_t VPORT_CONTROL = 2000;      ///< Control messages (server->client)
    static constexpr uint16_t VPORT_BACKCHANNEL = 3000;  ///< Backchannel (client->server)

    /// Dual RIST contexts for bidirectional communication
    struct rist_ctx* receiver_ctx_{nullptr};     ///< RIST receiver context (audio/control from server)
    struct rist_peer* receiver_peer_{nullptr};   ///< RIST receiver peer
    struct rist_ctx* sender_ctx_{nullptr};       ///< RIST sender context (backchannel to server)
    struct rist_peer* sender_peer_{nullptr};     ///< RIST sender peer
    
    std::atomic<bool> receiver_connected_{false}; ///< receiver connection status
    std::atomic<bool> sender_connected_{false};   ///< sender connection status
    std::atomic<bool> connected_{false};          ///< overall connection status
    std::atomic<bool> running_{false};            ///< receiver thread running
    std::thread receiver_thread_;                 ///< receiver thread
    
    /// Pending message handlers for audio/control data
    MessageHandler<msg::BaseMessage> pending_handler_;
    std::mutex handler_mutex_;                    ///< protect pending_handler_
    
    /// Buffer for message processing
    std::vector<uint8_t> buffer_;                 ///< buffer for received messages
    std::mutex buffer_mutex_;                     ///< protect buffer access
};

#endif // HAS_LIBRIST
