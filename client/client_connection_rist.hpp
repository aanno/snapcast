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

/// Hybrid RIST client connection
/**
 * Hybrid RIST client connection endpoint.
 * Receives media data via RIST protocol and uses TCP for control messages.
 */
class ClientConnectionRist : public ClientConnection
{
public:
    /// c'tor
    ClientConnectionRist(boost::asio::io_context& io_context, ClientSettings::Server server);
    /// d'tor
    virtual ~ClientConnectionRist();

    void disconnect() override;
    std::string getMacAddress() override;
    void getNextMessage(const MessageHandler<msg::BaseMessage>& handler) override;

private:
    boost::system::error_code doConnect(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint) override;
    void write(boost::asio::streambuf& buffer, WriteHandler&& write_handler) override;

    /// Initialize RIST receiver
    bool initRist();
    /// Cleanup RIST context
    void cleanupRist();
    /// RIST receiver thread
    void ristReceiverThread();
    /// RIST connection status callback
    static void connectionStatusCallback(void* arg, struct rist_peer* peer, enum rist_connection_status status);

private:
    /// TCP connection for control messages
    std::unique_ptr<ClientConnectionTcp> tcp_connection_;
    
    /// RIST receiver for media data
    struct rist_ctx* rist_ctx_{nullptr};     ///< RIST context
    struct rist_peer* rist_peer_{nullptr};   ///< RIST peer
    std::atomic<bool> rist_connected_{false}; ///< RIST connection status
    std::atomic<bool> running_{false};       ///< receiver thread running
    std::thread receiver_thread_;            ///< receiver thread
    
    /// Pending message handlers for media data
    MessageHandler<msg::BaseMessage> pending_handler_;
    std::mutex handler_mutex_;               ///< protect pending_handler_
};

#endif // HAS_LIBRIST