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
#include <mutex>
#include <vector>

/// Bidirectional RIST streaming endpoint for a connected client.
/**
 * Bidirectional RIST endpoint for a connected client using multiplexing.
 * Uses virtual ports to separate different data types:
 * - Audio data: virtual port 1000
 * - Control messages (server->client): virtual port 2000  
 * - Backchannel (client->server): virtual port 3000
 * Messages are sent to the client with the "send" method using RIST protocol.
 * Received messages from the client are passed to the StreamMessageReceiver callback
 */
class StreamSessionRistBidirectional : public StreamSession
{
public:
    /// ctor. Received message from the client are passed to StreamMessageReceiver
    StreamSessionRistBidirectional(StreamMessageReceiver* receiver, const ServerSettings& server_settings, 
                      const std::string& client_address, uint16_t client_port,
                      boost::asio::io_context& io_context);
    ~StreamSessionRistBidirectional() override;
    void start() override;
    void stop() override;
    std::string getIP() override;
    
    /// Set self-reference to keep session alive
    void setSelfReference(std::shared_ptr<StreamSessionRistBidirectional> self) { self_ = self; }

protected:
    /// Send message @p buffer using RIST and pass result to @p handler
    void sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler) override;

private:
    /// Initialize RIST sender and receiver contexts
    bool initRist();
    /// Cleanup RIST contexts
    void cleanupRist();
    /// Read messages from RIST receiver (backchannel from client)
    void readNext();
    /// RIST receiver thread for backchannel messages
    void ristReceiverThread();
    /// RIST connection status callback for sender
    static void senderConnectionStatusCallback(void* arg, struct rist_peer* peer, enum rist_connection_status status);
    /// RIST connection status callback for receiver
    static void receiverConnectionStatusCallback(void* arg, struct rist_peer* peer, enum rist_connection_status status);
    /// Determine virtual port based on message type
    uint16_t getVirtualPortForMessage(const shared_const_buffer& buffer);

private:
    // Virtual ports for multiplexing
    static constexpr uint16_t VPORT_AUDIO = 1000;        ///< Audio data port
    static constexpr uint16_t VPORT_CONTROL = 2000;      ///< Control messages (server->client)
    static constexpr uint16_t VPORT_BACKCHANNEL = 3000;  ///< Backchannel (client->server)

    struct rist_ctx* sender_ctx_{nullptr};      ///< RIST sender context for sending audio/control to clients
    struct rist_peer* sender_peer_{nullptr};    ///< RIST sender peer
    struct rist_ctx* receiver_ctx_{nullptr};    ///< RIST receiver context for receiving backchannel from clients
    struct rist_peer* receiver_peer_{nullptr};  ///< RIST receiver peer
    
    std::atomic<bool> sender_connected_{false}; ///< sender connection status
    std::atomic<bool> receiver_connected_{false}; ///< receiver connection status
    
    std::string client_address_;                 ///< client IP address  
    uint16_t client_port_;                       ///< client port
    std::atomic<bool> connected_{false};         ///< connection status
    std::atomic<bool> running_{false};       ///< running status
    std::thread receiver_thread_;            ///< thread for reading backchannel messages
    
    // Buffer for message processing
    std::vector<uint8_t> buffer_;            ///< buffer for received messages
    mutable std::mutex buffer_mutex_;        ///< protect buffer access
    
    /// Self-reference to keep session alive (RIST sessions are not connection-driven like TCP)
    std::shared_ptr<StreamSessionRistBidirectional> self_;
};