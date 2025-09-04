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
#include "common/message/message.hpp"

// standard headers
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

static constexpr auto LOG_TAG = "ConnectionRISTBi";

ClientConnectionRistBidirectional::ClientConnectionRistBidirectional(boost::asio::io_context& io_context, ClientSettings::Server server)
    : ClientConnection(io_context, std::move(server))
{
    LOG(INFO, LOG_TAG) << "Creating bidirectional RIST client connection\n";
    buffer_.resize(8192); // Initial buffer size
}

ClientConnectionRistBidirectional::~ClientConnectionRistBidirectional()
{
    LOG(DEBUG, LOG_TAG) << "~ClientConnectionRistBidirectional\n";
    disconnect();
}

boost::system::error_code ClientConnectionRistBidirectional::doConnect(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint)
{
    LOG(INFO, LOG_TAG) << "Connecting to bidirectional RIST server: " << endpoint.address().to_string() << ":" << endpoint.port() << "\n";

    if (!initRist())
    {
        LOG(ERROR, LOG_TAG) << "Failed to initialize bidirectional RIST\n";
        return boost::system::errc::make_error_code(boost::system::errc::connection_refused);
    }

    connected_ = true;
    running_ = true;

    // Start receiver thread
    receiver_thread_ = std::thread(&ClientConnectionRistBidirectional::ristReceiverThread, this);

    LOG(INFO, LOG_TAG) << "Bidirectional RIST client connection established\n";
    return boost::system::error_code();
}

void ClientConnectionRistBidirectional::disconnect()
{
    LOG(DEBUG, LOG_TAG) << "Disconnecting bidirectional RIST client\n";
    
    running_ = false;
    connected_ = false;

    if (receiver_thread_.joinable())
    {
        receiver_thread_.join();
    }

    cleanupRist();
    LOG(DEBUG, LOG_TAG) << "Bidirectional RIST client disconnected\n";
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
    LOG(INFO, LOG_TAG) << "My MAC: \"" << mac << "\", host: " << server_.host << "\n";
    return mac;
}

void ClientConnectionRistBidirectional::getNextMessage(const MessageHandler<msg::BaseMessage>& handler)
{
    std::lock_guard<std::mutex> lock(handler_mutex_);
    pending_handler_ = handler;
}

void ClientConnectionRistBidirectional::write(boost::asio::streambuf& buffer, WriteHandler&& write_handler)
{
    if (!sender_ctx_ || !sender_connected_) {
        LOG(WARNING, LOG_TAG) << "Cannot send backchannel data - RIST sender not connected\n";
        write_handler(boost::system::error_code(boost::asio::error::not_connected), 0);
        return;
    }

    // Get data from streambuf
    const auto* data_ptr = boost::asio::buffer_cast<const char*>(buffer.data());
    size_t data_size = buffer.size();

    if (data_size == 0) {
        LOG(WARNING, LOG_TAG) << "Attempting to send empty buffer\n";
        write_handler(boost::system::error_code(), 0);
        return;
    }

    // Create RIST data block for backchannel
    struct rist_data_block data_block = {};
    data_block.payload = data_ptr;
    data_block.payload_len = data_size;
    data_block.ts_ntp = 0; // Let librist populate timestamp
    data_block.virt_src_port = VPORT_BACKCHANNEL;
    data_block.virt_dst_port = VPORT_BACKCHANNEL;

    // Send data via RIST sender context
    int ret = rist_sender_data_write(sender_ctx_, &data_block);
    if (ret < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to send backchannel data via RIST: " << ret << "\n";
        write_handler(boost::system::error_code(boost::asio::error::broken_pipe), 0);
        return;
    }

    LOG(DEBUG, LOG_TAG) << "Sent " << data_size << " bytes via RIST backchannel\n";
    write_handler(boost::system::error_code(), data_size);
}

bool ClientConnectionRistBidirectional::initRist()
{
    LOG(INFO, LOG_TAG) << "Initializing bidirectional RIST client\n";
    LOG(INFO, LOG_TAG) << "DEBUG: Server host = " << server_.host << "\n";
    LOG(INFO, LOG_TAG) << "DEBUG: Server port = " << server_.port << "\n";

    // Create RIST receiver context for receiving audio/control from server
    int ret = rist_receiver_create(&receiver_ctx_, RIST_PROFILE_MAIN, nullptr);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST receiver context: " << ret << "\n";
        return false;
    }

    // Create RIST sender context for sending backchannel to server
    ret = rist_sender_create(&sender_ctx_, RIST_PROFILE_MAIN, 0, nullptr);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST sender context: " << ret << "\n";
        rist_destroy(receiver_ctx_);
        receiver_ctx_ = nullptr;
        return false;
    }

    // Set connection status callbacks
    ret = rist_connection_status_callback_set(receiver_ctx_, receiverConnectionStatusCallback, this);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to set receiver connection status callback: " << ret << "\n";
        cleanupRist();
        return false;
    }

    ret = rist_connection_status_callback_set(sender_ctx_, senderConnectionStatusCallback, this);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to set sender connection status callback: " << ret << "\n";
        cleanupRist();
        return false;
    }

    // Configure receiver to connect to server's sender (main port)
    std::string receiver_url = "rist://" + server_.host + ":" + std::to_string(server_.port);
    LOG(INFO, LOG_TAG) << "Using RIST receiver URL: " << receiver_url << "\n";

    struct rist_peer_config* receiver_config = nullptr;
    ret = rist_parse_address2(receiver_url.c_str(), &receiver_config);
    if (ret < 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to parse RIST receiver URL: " << receiver_url << ", error: " << ret << "\n";
        cleanupRist();
        return false;
    }

    ret = rist_peer_create(receiver_ctx_, &receiver_peer_, receiver_config);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST receiver peer: " << ret << "\n";
        free(receiver_config);
        cleanupRist();
        return false;
    }
    free(receiver_config);

    // Configure sender to connect to server's backchannel receiver (port + 1)
    uint16_t backchannel_port = server_.port + 1;
    std::string sender_url = "rist://" + server_.host + ":" + std::to_string(backchannel_port);
    LOG(INFO, LOG_TAG) << "Using RIST sender URL: " << sender_url << "\n";

    struct rist_peer_config* sender_config = nullptr;
    ret = rist_parse_address2(sender_url.c_str(), &sender_config);
    if (ret < 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to parse RIST sender URL: " << sender_url << ", error: " << ret << "\n";
        cleanupRist();
        return false;
    }

    ret = rist_peer_create(sender_ctx_, &sender_peer_, sender_config);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST sender peer: " << ret << "\n";
        free(sender_config);
        cleanupRist();
        return false;
    }
    free(sender_config);

    // Start both contexts
    ret = rist_start(receiver_ctx_);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to start RIST receiver context: " << ret << "\n";
        cleanupRist();
        return false;
    }

    ret = rist_start(sender_ctx_);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to start RIST sender context: " << ret << "\n";
        cleanupRist();
        return false;
    }

    LOG(INFO, LOG_TAG) << "Bidirectional RIST client initialized successfully\n";
    return true;
}

void ClientConnectionRistBidirectional::cleanupRist()
{
    if (receiver_ctx_)
    {
        LOG(DEBUG, LOG_TAG) << "Cleaning up RIST receiver context\n";
        rist_destroy(receiver_ctx_);
        receiver_ctx_ = nullptr;
        receiver_peer_ = nullptr;
    }
    
    if (sender_ctx_)
    {
        LOG(DEBUG, LOG_TAG) << "Cleaning up RIST sender context\n";
        rist_destroy(sender_ctx_);
        sender_ctx_ = nullptr;
        sender_peer_ = nullptr;
    }
}

void ClientConnectionRistBidirectional::ristReceiverThread()
{
    LOG(INFO, LOG_TAG) << "Starting bidirectional RIST receiver thread\n";

    while (running_)
    {
        if (!receiver_ctx_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        struct rist_data_block* data_block = nullptr;
        int queue_length = rist_receiver_data_read2(receiver_ctx_, &data_block, 100); // 100ms timeout
        
        if (queue_length == 0) {
            // No data available, continue
            continue;
        } else if (queue_length < 0) {
            LOG(DEBUG, LOG_TAG) << "RIST receiver error or timeout: " << queue_length << "\n";
        }

        if (queue_length > 0 && data_block)
        {
            try
            {
                LOG(DEBUG, LOG_TAG) << "Received data: " << data_block->payload_len 
                                   << " bytes on virtual port " << data_block->virt_dst_port << "\n";

                // Process audio and control messages (ignore backchannel messages)
                if (data_block->virt_dst_port == VPORT_AUDIO || data_block->virt_dst_port == VPORT_CONTROL)
                {
                    // Ensure we have enough buffer space
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    if (buffer_.size() < data_block->payload_len)
                    {
                        buffer_.resize(data_block->payload_len);
                    }

                    // Copy data to our buffer
                    memcpy(buffer_.data(), data_block->payload, data_block->payload_len);

                    // Process the message
                    if (data_block->payload_len >= base_msg_size_)
                    {
                        base_message_.deserialize(reinterpret_cast<char*>(buffer_.data()));
                        
                        if (base_message_.type > message_type::kLast)
                        {
                            LOG(ERROR, LOG_TAG) << "Unknown message type received: " << base_message_.type << "\n";
                        }
                        else if (base_message_.size > msg::max_size)
                        {
                            LOG(ERROR, LOG_TAG) << "Message too large: " << base_message_.size << "\n";
                        }
                        else if (base_message_.size <= data_block->payload_len)
                        {
                            // We have a complete message
                            auto message = msg::factory::createMessage(base_message_, reinterpret_cast<char*>(buffer_.data()));
                            
                            std::lock_guard<std::mutex> handler_lock(handler_mutex_);
                            if (pending_handler_)
                            {
                                messageReceived(std::move(message), pending_handler_);
                                pending_handler_ = nullptr;
                            }
                        }
                    }
                }

                // Free the data block
                rist_receiver_data_block_free2(&data_block);
            }
            catch (const std::exception& e)
            {
                LOG(ERROR, LOG_TAG) << "Exception in RIST receiver thread: " << e.what() << "\n";
                if (data_block)
                    rist_receiver_data_block_free2(&data_block);
            }
        }
        else if (queue_length < 0)
        {
            LOG(ERROR, LOG_TAG) << "RIST receiver error: " << queue_length << "\n";
            break;
        }
    }

    LOG(INFO, LOG_TAG) << "Bidirectional RIST receiver thread stopped\n";
}

void ClientConnectionRistBidirectional::receiverConnectionStatusCallback(void* arg, struct rist_peer* /*peer*/, enum rist_connection_status status)
{
    auto* client = static_cast<ClientConnectionRistBidirectional*>(arg);
    if (!client)
    {
        return;
    }

    switch (status)
    {
        case RIST_CONNECTION_ESTABLISHED:
            LOG(INFO, LOG_TAG) << "RIST receiver connection established\n";
            client->receiver_connected_ = true;
            client->connected_ = client->receiver_connected_ && client->sender_connected_;
            break;
        case RIST_CONNECTION_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST receiver connection timed out\n";
            client->receiver_connected_ = false;
            client->connected_ = false;
            break;
        case RIST_CLIENT_CONNECTED:
            LOG(INFO, LOG_TAG) << "RIST receiver client connected\n";
            client->receiver_connected_ = true;
            client->connected_ = client->receiver_connected_ && client->sender_connected_;
            break;
        case RIST_CLIENT_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST receiver client timed out\n";
            client->receiver_connected_ = false;
            client->connected_ = false;
            break;
        default:
            LOG(WARNING, LOG_TAG) << "Unknown RIST receiver connection status: " << status << "\n";
            break;
    }
}

void ClientConnectionRistBidirectional::senderConnectionStatusCallback(void* arg, struct rist_peer* /*peer*/, enum rist_connection_status status)
{
    auto* client = static_cast<ClientConnectionRistBidirectional*>(arg);
    if (!client)
    {
        return;
    }

    switch (status)
    {
        case RIST_CONNECTION_ESTABLISHED:
            LOG(INFO, LOG_TAG) << "RIST sender connection established\n";
            client->sender_connected_ = true;
            client->connected_ = client->receiver_connected_ && client->sender_connected_;
            break;
        case RIST_CONNECTION_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST sender connection timed out\n";
            client->sender_connected_ = false;
            client->connected_ = false;
            break;
        case RIST_CLIENT_CONNECTED:
            LOG(INFO, LOG_TAG) << "RIST sender client connected\n";
            client->sender_connected_ = true;
            client->connected_ = client->receiver_connected_ && client->sender_connected_;
            break;
        case RIST_CLIENT_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST sender client timed out\n";
            client->sender_connected_ = false;
            client->connected_ = false;
            break;
        default:
            LOG(WARNING, LOG_TAG) << "Unknown RIST sender connection status: " << status << "\n";
            break;
    }
}

#endif // HAS_LIBRIST