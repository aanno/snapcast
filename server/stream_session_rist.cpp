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

// prototype/interface header file
#include "stream_session_rist.hpp"

// local headers
#include "common/aixlog.hpp"

// standard headers
#include <iostream>
#include <cstring>

using namespace std;
using namespace streamreader;

static constexpr auto LOG_TAG = "StreamSessionRIST";

StreamSessionRist::StreamSessionRist(StreamMessageReceiver* receiver, const ServerSettings& server_settings,
                                     const std::string& client_address, uint16_t client_port,
                                     boost::asio::io_context& io_context)
    : StreamSession(io_context.get_executor(), server_settings, receiver),
      client_address_(client_address), client_port_(client_port)
{
    LOG(INFO, LOG_TAG) << "Creating RIST session for client: " << client_address_ << ":" << client_port_ << "\n";
}

StreamSessionRist::~StreamSessionRist()
{
    LOG(DEBUG, LOG_TAG) << "~StreamSessionRist\n";
    stop();
}

bool StreamSessionRist::initRist()
{
    LOG(INFO, LOG_TAG) << "Initializing RIST logging\n";

    rist_logging_settings log_settings = {};
    log_settings.log_level = RIST_LOG_DEBUG; // Set debug level
    log_settings.log_stream = stdout; // Output to stdout
    rist_logging_set_global(&log_settings);

    LOG(INFO, LOG_TAG) << "Initializing RIST sender\n";
    
    // Create RIST logging settings - pass nullptr for default logging
    int ret = rist_sender_create(&rist_ctx_, RIST_PROFILE_MAIN, 0, nullptr);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST sender context: " << ret << "\n";
        return false;
    }

    // Set connection status callback
    ret = rist_connection_status_callback_set(rist_ctx_, connectionStatusCallback, this);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to set connection status callback: " << ret << "\n";
        rist_destroy(rist_ctx_);
        rist_ctx_ = nullptr;
        return false;
    }

    // Create URL string for librist to parse
    std::string rist_url = "rist://" + client_address_ + ":" + std::to_string(client_port_);
    LOG(INFO, LOG_TAG) << "Using RIST URL: " << rist_url << "\n";

    // Parse the RIST URL
    struct rist_peer_config* peer_config = nullptr;
    ret = rist_parse_address2(rist_url.c_str(), &peer_config);
    if (ret < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to parse RIST URL: " << rist_url << ", error: " << ret << "\n";
        rist_destroy(rist_ctx_);
        rist_ctx_ = nullptr;
        return false;
    }

    // Create peer
    ret = rist_peer_create(rist_ctx_, &rist_peer_, peer_config);
    if (ret != 0) {
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
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to start RIST context: " << ret << "\n";
        rist_destroy(rist_ctx_);
        rist_ctx_ = nullptr;
        rist_peer_ = nullptr;
        return false;
    }

    LOG(INFO, LOG_TAG) << "RIST sender initialized successfully\n";
    return true;
}

void StreamSessionRist::cleanupRist()
{
    if (rist_ctx_) {
        LOG(DEBUG, LOG_TAG) << "Cleaning up RIST context\n";
        rist_destroy(rist_ctx_);
        rist_ctx_ = nullptr;
        rist_peer_ = nullptr;
    }
}

void StreamSessionRist::start()
{
    LOG(INFO, LOG_TAG) << "Starting RIST session\n";
    
    if (!initRist()) {
        LOG(ERROR, LOG_TAG) << "Failed to initialize RIST\n";
        if (messageReceiver_) {
            messageReceiver_->onDisconnect(this);
        }
        return;
    }

    running_ = true;
    
    // For now, we don't need a read thread since RIST is primarily for sending
    // In the future, we could add bidirectional communication support here
    readNext();
}

void StreamSessionRist::stop()
{
    LOG(DEBUG, LOG_TAG) << "Stopping RIST session\n";
    
    running_ = false;
    connected_ = false;
    
    if (read_thread_.joinable()) {
        read_thread_.join();
    }
    
    cleanupRist();
    LOG(DEBUG, LOG_TAG) << "RIST session stopped\n";
}

std::string StreamSessionRist::getIP()
{
    return client_address_;
}

void StreamSessionRist::readNext()
{
    // For RIST streaming, we primarily send data to clients
    // This method is a placeholder for potential bidirectional communication
    // For now, we just simulate the reading process without actual data
    LOG(DEBUG, LOG_TAG) << "RIST readNext placeholder - streaming is primarily send-only\n";
}

void StreamSessionRist::sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler)
{
    if (!rist_ctx_ || !connected_) {
        LOG(WARNING, LOG_TAG) << "Cannot send data - RIST not connected\n";
        if (handler) {
            handler(boost::system::error_code(boost::asio::error::not_connected), 0);
        }
        return;
    }

    // Get buffer data
    const auto* data_ptr = boost::asio::buffer_cast<const char*>(*buffer.begin());
    size_t data_size = boost::asio::buffer_size(*buffer.begin());

    if (data_size == 0) {
        LOG(WARNING, LOG_TAG) << "Attempting to send empty buffer\n";
        if (handler) {
            handler(boost::system::error_code(), 0);
        }
        return;
    }

    // Create RIST data block
    struct rist_data_block data_block = {};
    data_block.payload = data_ptr;
    data_block.payload_len = data_size;
    data_block.ts_ntp = 0; // Let librist populate timestamp
    data_block.virt_src_port = client_port_;
    data_block.virt_dst_port = client_port_;

    // Send data via RIST
    int ret = rist_sender_data_write(rist_ctx_, &data_block);
    if (ret < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to send data via RIST: " << ret << "\n";
        if (handler) {
            handler(boost::system::error_code(boost::asio::error::broken_pipe), 0);
        }
        return;
    }

    LOG(DEBUG, LOG_TAG) << "Sent " << data_size << " bytes via RIST\n";
    
    if (handler) {
        handler(boost::system::error_code(), data_size);
    }
}

void StreamSessionRist::connectionStatusCallback(void* arg, struct rist_peer* /*peer*/, enum rist_connection_status status)
{
    auto* session = static_cast<StreamSessionRist*>(arg);
    if (!session) {
        return;
    }

    switch (status) {
        case RIST_CONNECTION_ESTABLISHED:
            LOG(INFO, LOG_TAG) << "RIST connection established\n";
            session->connected_ = true;
            break;
        case RIST_CONNECTION_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST connection timed out\n";
            session->connected_ = false;
            if (session->messageReceiver_) {
                session->messageReceiver_->onDisconnect(session);
            }
            break;
        case RIST_CLIENT_CONNECTED:
            LOG(INFO, LOG_TAG) << "RIST client connected\n";
            session->connected_ = true;
            break;
        case RIST_CLIENT_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST client timed out\n";
            session->connected_ = false;
            if (session->messageReceiver_) {
                session->messageReceiver_->onDisconnect(session);
            }
            break;
        default:
            LOG(WARNING, LOG_TAG) << "Unknown RIST connection status: " << status << "\n";
            break;
    }
}
