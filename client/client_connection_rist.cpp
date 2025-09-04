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
#include "client_connection_rist.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/utils.hpp"

// standard headers
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

static constexpr auto LOG_TAG = "ConnectionRIST";
static constexpr auto LOG_LIBRIST_TAG = "libRIST";

static int rist_log_callback(void* arg, enum rist_log_level level, const char* msg) {
    (void)arg;
    fprintf(stdout, "[RIST] [%d] %s", level, msg);
    switch (level) {
        case RIST_LOG_ERROR:
            LOG(ERROR, LOG_LIBRIST_TAG) << msg << "\n";
            break;
        case RIST_LOG_WARN:
            LOG(WARNING, LOG_LIBRIST_TAG) << msg << "\n";
            break;
        case RIST_LOG_INFO:
            LOG(INFO, LOG_LIBRIST_TAG) << msg << "\n";
            break;
        case RIST_LOG_DEBUG:
            LOG(DEBUG, LOG_LIBRIST_TAG) << msg << "\n";
            break;
        default:
            LOG(DEBUG, LOG_LIBRIST_TAG) << msg << "\n";
            break;
    }
    return 0;
}

ClientConnectionRist::ClientConnectionRist(boost::asio::io_context& io_context, ClientSettings::Server server)
    : ClientConnection(io_context, std::move(server))
{
    LOG(INFO, LOG_TAG) << "Creating RIST client connection\n";
    buffer_.resize(8192); // Initial buffer size
}

ClientConnectionRist::~ClientConnectionRist()
{
    LOG(DEBUG, LOG_TAG) << "~ClientConnectionRist\n";
    disconnect();
}

boost::system::error_code ClientConnectionRist::doConnect(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint)
{
    LOG(INFO, LOG_TAG) << "Connecting to RIST server: " << endpoint.address().to_string() << ":" << endpoint.port() << "\n";

    if (!initRist())
    {
        LOG(ERROR, LOG_TAG) << "Failed to initialize RIST receiver\n";
        return boost::system::errc::make_error_code(boost::system::errc::connection_refused);
    }

    connected_ = true;
    running_ = true;

    // Start receiver thread
    receiver_thread_ = std::thread(&ClientConnectionRist::ristReceiverThread, this);

    LOG(INFO, LOG_TAG) << "RIST client connection established\n";
    return boost::system::error_code();
}

void ClientConnectionRist::disconnect()
{
    LOG(DEBUG, LOG_TAG) << "Disconnecting RIST client\n";
    
    running_ = false;
    connected_ = false;

    if (receiver_thread_.joinable())
    {
        receiver_thread_.join();
    }

    cleanupRist();
    LOG(DEBUG, LOG_TAG) << "RIST client disconnected\n";
}

std::string ClientConnectionRist::getMacAddress()
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

void ClientConnectionRist::getNextMessage(const MessageHandler<msg::BaseMessage>& handler)
{
    std::lock_guard<std::mutex> lock(handler_mutex_);
    pending_handler_ = handler;
}

void ClientConnectionRist::write(boost::asio::streambuf& /* buffer */, WriteHandler&& write_handler)
{
    // RIST is primarily a receiver protocol for the client
    // For bidirectional communication, we would implement RIST sender here
    // For now, we just indicate success since control messages typically use TCP
    LOG(DEBUG, LOG_TAG) << "RIST client write not implemented (RIST is receive-only)\n";
    write_handler(boost::system::error_code(), 0);
}

bool ClientConnectionRist::initRist()
{
    LOG(INFO, LOG_TAG) << "Initializing RIST logging\n";

    log_settings_ = {};
    log_settings_.log_level = RIST_LOG_DEBUG; // Set debug level
    log_settings_.log_stream = nullptr; // stdout; // Output to stdout
    log_settings_.log_cb = rist_log_callback; // Set callback
    log_settings_.log_cb_arg = nullptr; // Optional user data (set if needed)
    rist_logging_set_global(&log_settings_);

    LOG(INFO, LOG_TAG) << "Initializing RIST receiver\n";

    // Create RIST receiver with main profile
    int ret = rist_receiver_create(&rist_ctx_, RIST_PROFILE_MAIN, &log_settings_);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST receiver context: " << ret << "\n";
        return false;
    }

    // Set connection status callback
    ret = rist_connection_status_callback_set(rist_ctx_, connectionStatusCallback, this);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to set connection status callback: " << ret << "\n";
        rist_destroy(rist_ctx_);
        rist_ctx_ = nullptr;
        return false;
    }

    // Create URL string for librist to parse
    std::string rist_url = "rist://" + server_.host + ":" + std::to_string(server_.port);
    LOG(INFO, LOG_TAG) << "Using RIST URL: " << rist_url << "\n";

    // Parse the RIST URL
    struct rist_peer_config* peer_config = nullptr;
    ret = rist_parse_address2(rist_url.c_str(), &peer_config);
    if (ret < 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to parse RIST URL: " << rist_url << ", error: " << ret << "\n";
        rist_destroy(rist_ctx_);
        rist_ctx_ = nullptr;
        return false;
    }

    // Create peer
    ret = rist_peer_create(rist_ctx_, &rist_peer_, peer_config);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST peer: " << ret << "\n";
        free(peer_config);
        rist_destroy(rist_ctx_);
        rist_ctx_ = nullptr;
        return false;
    }

    // Free the peer config as it's been copied
    free(peer_config);

    // Start RIST context
    ret = rist_start(rist_ctx_);
    if (ret != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to start RIST context: " << ret << "\n";
        rist_destroy(rist_ctx_);
        rist_ctx_ = nullptr;
        rist_peer_ = nullptr;
        return false;
    }

    LOG(INFO, LOG_TAG) << "RIST receiver initialized successfully\n";
    return true;
}

void ClientConnectionRist::cleanupRist()
{
    if (rist_ctx_)
    {
        LOG(DEBUG, LOG_TAG) << "Cleaning up RIST context\n";
        rist_destroy(rist_ctx_);
        rist_ctx_ = nullptr;
        rist_peer_ = nullptr;
    }
}

void ClientConnectionRist::ristReceiverThread()
{
    LOG(INFO, LOG_TAG) << "Starting RIST receiver thread\n";

    while (running_)
    {
        if (!rist_ctx_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        struct rist_data_block* data_block = nullptr;
        int queue_length = rist_receiver_data_read2(rist_ctx_, &data_block, 100); // 100ms timeout

        if (queue_length > 0 && data_block)
        {
            try
            {
                // Ensure we have enough buffer space
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
                        
                        std::lock_guard<std::mutex> lock(handler_mutex_);
                        if (pending_handler_)
                        {
                            messageReceived(std::move(message), pending_handler_);
                            pending_handler_ = nullptr;
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

    LOG(INFO, LOG_TAG) << "RIST receiver thread stopped\n";
}

void ClientConnectionRist::connectionStatusCallback(void* arg, struct rist_peer* /*peer*/, enum rist_connection_status status)
{
    auto* client = static_cast<ClientConnectionRist*>(arg);
    if (!client)
    {
        return;
    }

    switch (status)
    {
        case RIST_CONNECTION_ESTABLISHED:
            LOG(INFO, LOG_TAG) << "RIST connection established\n";
            client->connected_ = true;
            break;
        case RIST_CONNECTION_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST connection timed out\n";
            client->connected_ = false;
            break;
        case RIST_CLIENT_CONNECTED:
            LOG(INFO, LOG_TAG) << "RIST client connected\n";
            client->connected_ = true;
            break;
        case RIST_CLIENT_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST client timed out\n";
            client->connected_ = false;
            break;
        default:
            LOG(WARNING, LOG_TAG) << "Unknown RIST connection status: " << status << "\n";
            break;
    }
}

#endif // HAS_LIBRIST
