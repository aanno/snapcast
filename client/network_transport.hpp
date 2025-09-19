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
#include "common/message/message.hpp"

// 3rd party headers
#include <boost/system/error_code.hpp>

// standard headers
#include <functional>
#include <memory>
#include <string>

namespace client
{

/**
 * Pure Network Transport Interface
 * 
 * Defines clean contract for network transport layer with no protocol knowledge.
 * Transport implementations handle raw network operations (connect, send, receive)
 * while protocol processing is handled by higher layers.
 */
class NetworkTransport
{
public:
    // Callback types for transport operations
    using ConnectCallback = std::function<void(const boost::system::error_code&)>;
    using SendCallback = std::function<void(const boost::system::error_code&)>;
    using MessageCallback = std::function<void(const boost::system::error_code&, std::unique_ptr<msg::BaseMessage>)>;

    virtual ~NetworkTransport() = default;

    // ============ Connection Management ============
    
    /**
     * Establish network connection
     * @param callback Called when connection attempt completes
     */
    virtual void connect(ConnectCallback callback) = 0;
    
    /**
     * Close network connection
     */
    virtual void disconnect() = 0;

    // ============ Data Transfer ============
    
    /**
     * Send message over network
     * @param message Message to send 
     * @param callback Called when send operation completes
     */
    virtual void send(std::shared_ptr<msg::BaseMessage> message, SendCallback callback) = 0;
    
    /**
     * Start receiving next message from transport
     * @param callback Called when next message is received from network
     */
    virtual void receiveMessage(MessageCallback callback) = 0;

    // ============ Network Information ============
    
    /**
     * Get network interface MAC address
     * @return MAC address string for client identification
     */
    virtual std::string getMacAddress() = 0;

protected:
    // Protected constructor - this is an interface class
    NetworkTransport() = default;
};

} // namespace client