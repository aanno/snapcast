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
#include "common/rist_transport.hpp"
#include "common/message/hello.hpp"

// standard headers
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>

using namespace std;

static constexpr auto LOG_TAG = "ConnectionRISTBi";

ClientConnectionRistBidirectional::ClientConnectionRistBidirectional(boost::asio::io_context& io_context, ClientSettings::Server server)
    : ClientConnection(io_context, std::move(server)), running_(false)
{
    LOG(INFO, LOG_TAG) << "Creating RIST client connection with RistTransport integration\n";
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
    connected_ = true;

    // Start message processing thread
    message_thread_ = std::thread(&ClientConnectionRistBidirectional::messageProcessorThread, this);

    // Send Hello message to initiate handshake
    sendHello();

    LOG(INFO, LOG_TAG) << "RIST client connection established\n";
    return boost::system::error_code();
}

void ClientConnectionRistBidirectional::disconnect()
{
    LOG(DEBUG, LOG_TAG) << "Disconnecting RIST client\n";
    
    running_ = false;
    connected_ = false;

    if (rist_transport_)
    {
        rist_transport_->stop();
        rist_transport_.reset();
    }

    queue_cv_.notify_all();
    
    if (message_thread_.joinable())
    {
        message_thread_.join();
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
    
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        pending_handler_ = handler;
    }
    
    queue_cv_.notify_one();
}

void ClientConnectionRistBidirectional::write(boost::asio::streambuf& buffer, WriteHandler&& write_handler)
{
    if (!rist_transport_)
    {
        LOG(WARNING, LOG_TAG) << "Cannot send data - RIST transport not available\n";
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

    // Create message from buffer data
    msg::BaseMessage base_message;
    base_message.deserialize(const_cast<char*>(data_ptr));
    
    auto message = msg::factory::createMessage(base_message, const_cast<char*>(data_ptr) + sizeof(msg::BaseMessage));
    if (!message)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create message for sending\n";
        write_handler(boost::system::error_code(boost::asio::error::invalid_argument), 0);
        return;
    }

    // Send via RIST backchannel
    if (rist_transport_->sendMessage(RistTransport::VPORT_BACKCHANNEL, *message))
    {
        LOG(DEBUG, LOG_TAG) << "Sent " << data_size << " bytes via RIST backchannel\n";
        write_handler(boost::system::error_code(), data_size);
    }
    else
    {
        LOG(ERROR, LOG_TAG) << "Failed to send data via RIST\n";
        write_handler(boost::system::error_code(boost::asio::error::broken_pipe), 0);
    }
}

// RistTransportReceiver interface
void ClientConnectionRistBidirectional::onRistMessageReceived(const msg::BaseMessage& baseMessage, const std::string& payload, uint16_t vport)
{
    LOG(DEBUG, LOG_TAG) << "RIST message received: type=" << baseMessage.type << ", vport=" << vport << "\n";
    
    try
    {
        // Create message from received data
        auto message = msg::factory::createMessage(baseMessage, const_cast<char*>(payload.data()));
        if (message)
        {
            // Set received timestamp
            tv now;
            message->received = now;
            
            // Queue message for processing
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                message_queue_.push(std::move(message));
            }
            queue_cv_.notify_one();
        }
        else
        {
            LOG(ERROR, LOG_TAG) << "Failed to create message from RIST data\n";
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
    connected_ = true;
}

void ClientConnectionRistBidirectional::onRistClientDisconnected(const std::string& clientId)
{
    LOG(INFO, LOG_TAG) << "RIST connection lost: " << clientId << "\n";
    connected_ = false;
}

void ClientConnectionRistBidirectional::sendHello()
{
    if (!rist_transport_)
        return;

    try
    {
        msg::Hello hello;
        hello.MAC = getMacAddress();
        hello.hostname = boost::asio::ip::host_name();
        hello.version = VERSION;
        hello.clientName = "Snapclient";
        hello.os = OS;
        hello.arch = ARCH;
        hello.instance = 1;
        hello.uuid = getMacAddress(); // Use MAC as UUID for simplicity
        
        if (rist_transport_->sendMessage(RistTransport::VPORT_BACKCHANNEL, hello))
        {
            LOG(INFO, LOG_TAG) << "Sent Hello message to server\n";
        }
        else
        {
            LOG(ERROR, LOG_TAG) << "Failed to send Hello message\n";
        }
    }
    catch (const std::exception& e)
    {
        LOG(ERROR, LOG_TAG) << "Error sending Hello message: " << e.what() << "\n";
    }
}

void ClientConnectionRistBidirectional::messageProcessorThread()
{
    LOG(INFO, LOG_TAG) << "Starting RIST message processor thread\n";
    
    while (running_)
    {
        std::unique_ptr<msg::BaseMessage> message;
        
        // Wait for message or handler
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { 
                return !message_queue_.empty() || !running_;
            });
            
            if (!running_)
                break;
                
            if (message_queue_.empty())
                continue;
                
            message = std::move(message_queue_.front());
            message_queue_.pop();
        }
        
        // Process message with handler
        MessageHandler<msg::BaseMessage> handler;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler = pending_handler_;
            pending_handler_ = nullptr;
        }
        
        if (handler && message)
        {
            LOG(DEBUG, LOG_TAG) << "Processing message type: " << message->type << "\n";
            handler(boost::system::error_code(), std::move(message));
        }
        else if (message)
        {
            LOG(WARNING, LOG_TAG) << "No handler available for message type: " << message->type << "\n";
        }
    }
    
    LOG(INFO, LOG_TAG) << "RIST message processor thread stopped\n";
}

#endif // HAS_LIBRIST