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

#ifdef HAS_LIBRIST

// prototype/interface header file
#include "client_connection_rist_bidirectional.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/utils.hpp"
#include "common/message/factory.hpp"

// standard headers
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

static constexpr auto LOG_TAG = "ConnectionRISTBi";

ClientConnectionRistBidirectional::ClientConnectionRistBidirectional(boost::asio::io_context& io_context, ClientSettings::Server server)
    : ClientConnection(io_context, std::move(server)), running_(false)
{
    LOG(INFO, LOG_TAG) << "Creating RIST client connection with RistTransport\n";
}

ClientConnectionRistBidirectional::~ClientConnectionRistBidirectional()
{
    LOG(DEBUG, LOG_TAG) << "~ClientConnectionRistBidirectional\n";
    disconnect();
}

boost::system::error_code ClientConnectionRistBidirectional::doConnect(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint)
{
    LOG(INFO, LOG_TAG) << "Connecting to RIST server: " << endpoint.address().to_string() << ":" << endpoint.port() << "\n";

    // Create and configure RIST transport in CLIENT mode
    rist_transport_ = std::make_unique<RistTransport>(RistTransport::Mode::CLIENT, this);
    
    if (!rist_transport_->configureClient(endpoint.address().to_string(), endpoint.port()))
    {
        LOG(ERROR, LOG_TAG) << "Failed to configure RIST client\n";
        return boost::system::errc::make_error_code(boost::system::errc::connection_refused);
    }

    if (!rist_transport_->start())
    {
        LOG(ERROR, LOG_TAG) << "Failed to start RIST client\n";
        return boost::system::errc::make_error_code(boost::system::errc::connection_refused);
    }

    running_ = true;
    LOG(INFO, LOG_TAG) << "RIST client connection established\n";
    return boost::system::error_code();
}

void ClientConnectionRistBidirectional::disconnect()
{
    LOG(DEBUG, LOG_TAG) << "Disconnecting RIST client\n";
    
    running_ = false;

    if (rist_transport_)
    {
        rist_transport_->stop();
        rist_transport_.reset();
    }

    LOG(DEBUG, LOG_TAG) << "RIST client disconnected\n";
}

std::string ClientConnectionRistBidirectional::getMacAddress()
{
    // For RIST, we don't have a socket, so create a temporary one to get MAC
    int temp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    std::string mac = ::getMacAddress(temp_socket);
    if (temp_socket >= 0)
        close(temp_socket);
    
    if (mac.empty())
        mac = "00:00:00:00:00:00";
    LOG(DEBUG, LOG_TAG) << "My MAC: \"" << mac << "\", host: " << server_.host << "\n";
    return mac;
}

void ClientConnectionRistBidirectional::getNextMessage(const MessageHandler<msg::BaseMessage>& handler)
{
    LOG(DEBUG, LOG_TAG) << "getNextMessage called\n";
    
    // Store the handler for when we receive the next message
    std::lock_guard<std::mutex> lock(next_message_mutex_);
    next_message_handler_ = handler;
}

void ClientConnectionRistBidirectional::write(boost::asio::streambuf& buffer, WriteHandler&& write_handler)
{
    if (!rist_transport_)
    {
        LOG(ERROR, LOG_TAG) << "Cannot send data - RIST transport not available\n";
        write_handler(boost::system::error_code(boost::asio::error::not_connected), 0);
        return;
    }

    if (!running_)
    {
        LOG(ERROR, LOG_TAG) << "Cannot send data - RIST client not connected\n";
        write_handler(boost::system::error_code(boost::asio::error::not_connected), 0);
        return;
    }

    // Get data from streambuf
    const auto* data_ptr = boost::asio::buffer_cast<const char*>(buffer.data());
    size_t data_size = buffer.size();

    if (data_size == 0)
    {
        LOG(WARNING, LOG_TAG) << "Attempting to send empty buffer\n";
        write_handler(boost::system::error_code(), 0);
        return;
    }

    try {
        // Parse message header to get type and ID for logging (but don't re-serialize)
        msg::BaseMessage base_message;
        base_message.deserialize(const_cast<char*>(data_ptr));
        
        LOG(DEBUG, LOG_TAG) << "Sending message type " << base_message.type << " (id=" << base_message.id << ") via RIST backchannel\n";

        // Send raw data directly via RIST transport's sendRawData method
        bool success = rist_transport_->sendRawData(RistTransport::VPORT_BACKCHANNEL, data_ptr, data_size);
        if (success)
        {
            LOG(DEBUG, LOG_TAG) << "Successfully sent " << data_size << " bytes via RIST backchannel\n";
            write_handler(boost::system::error_code(), data_size);
        }
        else
        {
            LOG(ERROR, LOG_TAG) << "Failed to send " << data_size << " bytes via RIST backchannel\n";
            write_handler(boost::system::error_code(boost::asio::error::broken_pipe), 0);
        }
    }
    catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Exception while sending message via RIST: " << e.what() << "\n";
        write_handler(boost::system::error_code(boost::asio::error::invalid_argument), 0);
    }
}

// RistTransportReceiver interface
void ClientConnectionRistBidirectional::onRistMessageReceived(const msg::BaseMessage& baseMessage, const std::string& payload, uint16_t vport)
{
    LOG(DEBUG, LOG_TAG) << "RIST message received: type=" << baseMessage.type << " (id=" << baseMessage.id << "), vport=" << vport << "\n";
    
    try
    {
        // Set base_message_ to match what TCP client does - this is used by messageReceived() for correlation
        base_message_ = baseMessage;
        base_message_.received = tv{};
        
        // Create message from received data
        auto message = msg::factory::createMessage(baseMessage, const_cast<char*>(payload.data()));
        if (!message)
        {
            LOG(ERROR, LOG_TAG) << "Failed to create message from RIST data\n";
            return;
        }

        // Set received timestamp
        tv now;
        message->received = now;
        
        // Use the normal handler mechanism for all messages
        MessageHandler<msg::BaseMessage> handler;
        {
            std::lock_guard<std::mutex> lock(next_message_mutex_);
            handler = next_message_handler_;
            next_message_handler_ = nullptr;
        }

        if (handler)
        {
            LOG(DEBUG, LOG_TAG) << "Processing message type " << message->type << " through normal pipeline\n";
            messageReceived(std::move(message), handler);
        }
        else
        {
            LOG(WARNING, LOG_TAG) << "No handler available for RIST message type: " << message->type << "\n";
        }
    }
    catch (const std::exception& e)
    {
        LOG(ERROR, LOG_TAG) << "Error processing RIST message: " << e.what() << "\n";
    }
}

void ClientConnectionRistBidirectional::onRistClientConnected(const std::string& clientId)
{
    LOG(INFO, LOG_TAG) << "RIST connection established: " << clientId << "\n";
}

void ClientConnectionRistBidirectional::onRistClientDisconnected(const std::string& clientId)
{
    LOG(INFO, LOG_TAG) << "RIST connection lost: " << clientId << "\n";
}

#endif // HAS_LIBRIST