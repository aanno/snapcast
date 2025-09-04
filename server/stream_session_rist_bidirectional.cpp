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
#include "stream_session_rist_bidirectional.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/message/message.hpp"
#include "common/message/factory.hpp"

// standard headers
#include <iostream>
#include <cstring>

using namespace std;
using namespace streamreader;

static constexpr auto LOG_TAG = "StreamSessionRISTBi";

StreamSessionRistBidirectional::StreamSessionRistBidirectional(StreamMessageReceiver* receiver, const ServerSettings& server_settings,
                                     const std::string& client_address, uint16_t client_port,
                                     boost::asio::io_context& io_context)
    : StreamSession(io_context.get_executor(), server_settings, receiver),
      client_address_(client_address), client_port_(client_port)
{
    LOG(INFO, LOG_TAG) << "Creating bidirectional RIST session for client: " << client_address_ << ":" << client_port_ << "\n";
    buffer_.resize(8192); // Initial buffer size
}

StreamSessionRistBidirectional::~StreamSessionRistBidirectional()
{
    LOG(DEBUG, LOG_TAG) << "~StreamSessionRistBidirectional\n";
    stop();
}

bool StreamSessionRistBidirectional::initRist()
{
    LOG(INFO, LOG_TAG) << "Initializing bidirectional RIST server\n";
    
    // Create RIST sender context for sending audio/control to clients
    int ret = rist_sender_create(&sender_ctx_, RIST_PROFILE_MAIN, 0, nullptr);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST sender context: " << ret << "\n";
        return false;
    }

    // Create RIST receiver context for receiving backchannel from clients
    ret = rist_receiver_create(&receiver_ctx_, RIST_PROFILE_MAIN, nullptr);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST receiver context: " << ret << "\n";
        rist_destroy(sender_ctx_);
        sender_ctx_ = nullptr;
        return false;
    }

    // Set connection status callbacks
    ret = rist_connection_status_callback_set(sender_ctx_, senderConnectionStatusCallback, this);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to set sender connection status callback: " << ret << "\n";
        cleanupRist();
        return false;
    }

    ret = rist_connection_status_callback_set(receiver_ctx_, receiverConnectionStatusCallback, this);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to set receiver connection status callback: " << ret << "\n";
        cleanupRist();
        return false;
    }

    // Configure sender context to bind and accept client connections for sending audio/control
    std::string sender_url = "rist://@0.0.0.0:" + std::to_string(client_port_);
    LOG(INFO, LOG_TAG) << "Using RIST sender URL (bind): " << sender_url << "\n";

    struct rist_peer_config* sender_config = nullptr;
    ret = rist_parse_address2(sender_url.c_str(), &sender_config);
    if (ret < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to parse RIST sender URL: " << sender_url << ", error: " << ret << "\n";
        cleanupRist();
        return false;
    }

    ret = rist_peer_create(sender_ctx_, &sender_peer_, sender_config);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST sender peer: " << ret << "\n";
        free(sender_config);
        cleanupRist();
        return false;
    }
    free(sender_config);

    // Configure receiver context to bind on a different port for backchannel
    uint16_t backchannel_port = client_port_ + 1;  // Use next port for backchannel
    std::string receiver_url = "rist://@0.0.0.0:" + std::to_string(backchannel_port);
    LOG(INFO, LOG_TAG) << "Using RIST receiver URL (bind): " << receiver_url << "\n";

    struct rist_peer_config* receiver_config = nullptr;
    ret = rist_parse_address2(receiver_url.c_str(), &receiver_config);
    if (ret < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to parse RIST receiver URL: " << receiver_url << ", error: " << ret << "\n";
        cleanupRist();
        return false;
    }

    ret = rist_peer_create(receiver_ctx_, &receiver_peer_, receiver_config);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST receiver peer: " << ret << "\n";
        free(receiver_config);
        cleanupRist();
        return false;
    }
    free(receiver_config);

    // Start both contexts
    ret = rist_start(sender_ctx_);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to start RIST sender context: " << ret << "\n";
        cleanupRist();
        return false;
    }

    ret = rist_start(receiver_ctx_);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to start RIST receiver context: " << ret << "\n";
        cleanupRist();
        return false;
    }

    LOG(INFO, LOG_TAG) << "Bidirectional RIST server initialized successfully\n";
    return true;
}

void StreamSessionRistBidirectional::cleanupRist()
{
    if (sender_ctx_) {
        LOG(DEBUG, LOG_TAG) << "Cleaning up RIST sender context\n";
        rist_destroy(sender_ctx_);
        sender_ctx_ = nullptr;
        sender_peer_ = nullptr;
    }

    if (receiver_ctx_) {
        LOG(DEBUG, LOG_TAG) << "Cleaning up RIST receiver context\n";
        rist_destroy(receiver_ctx_);
        receiver_ctx_ = nullptr;
        receiver_peer_ = nullptr;
    }
}

void StreamSessionRistBidirectional::start()
{
    LOG(INFO, LOG_TAG) << "Starting bidirectional RIST session\n";
    
    if (!initRist()) {
        LOG(ERROR, LOG_TAG) << "Failed to initialize bidirectional RIST\n";
        if (messageReceiver_) {
            messageReceiver_->onDisconnect(this);
        }
        return;
    }

    running_ = true;
    
    // Start receiver thread for backchannel messages
    receiver_thread_ = std::thread(&StreamSessionRistBidirectional::ristReceiverThread, this);
    
    readNext();
}

void StreamSessionRistBidirectional::stop()
{
    LOG(DEBUG, LOG_TAG) << "Stopping bidirectional RIST session\n";
    
    running_ = false;
    connected_ = false;
    sender_connected_ = false;
    receiver_connected_ = false;
    
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
    }
    
    cleanupRist();
    
    // Clear self-reference to allow proper destruction
    self_.reset();
    
    LOG(DEBUG, LOG_TAG) << "Bidirectional RIST session stopped\n";
}

std::string StreamSessionRistBidirectional::getIP()
{
    return client_address_;
}

void StreamSessionRistBidirectional::readNext()
{
    // The actual reading is handled by the receiver thread
    LOG(DEBUG, LOG_TAG) << "Bidirectional RIST readNext - handled by receiver thread\n";
}

uint16_t StreamSessionRistBidirectional::getVirtualPortForMessage(const shared_const_buffer& buffer)
{
    // Check message type to determine virtual port
    if (buffer.begin() != buffer.end() && boost::asio::buffer_size(*buffer.begin()) >= sizeof(uint16_t)) {
        
        // Simple heuristic: if it looks like audio data (larger packets), use audio port
        // Otherwise use control port
        size_t data_size = boost::asio::buffer_size(*buffer.begin());
        
        if (data_size > 1024) {
            return VPORT_AUDIO;  // Large packets are likely audio
        } else {
            return VPORT_CONTROL; // Small packets are likely control messages
        }
    }
    
    return VPORT_CONTROL; // Default to control port
}

void StreamSessionRistBidirectional::sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler)
{
    if (!sender_ctx_ || !sender_connected_) {
        LOG(WARNING, LOG_TAG) << "Cannot send data - RIST sender not connected\n";
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

    // Determine virtual port based on message content
    uint16_t virt_port = getVirtualPortForMessage(buffer);

    // Create RIST data block
    struct rist_data_block data_block = {};
    data_block.payload = data_ptr;
    data_block.payload_len = data_size;
    data_block.ts_ntp = 0; // Let librist populate timestamp
    data_block.virt_src_port = virt_port;
    data_block.virt_dst_port = virt_port;

    // Send data via RIST sender context
    int ret = rist_sender_data_write(sender_ctx_, &data_block);
    if (ret < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to send data via RIST: " << ret << "\n";
        if (handler) {
            handler(boost::system::error_code(boost::asio::error::broken_pipe), 0);
        }
        return;
    }

    LOG(DEBUG, LOG_TAG) << "Sent " << data_size << " bytes via RIST on virtual port " << virt_port << "\n";
    
    if (handler) {
        handler(boost::system::error_code(), data_size);
    }
}

void StreamSessionRistBidirectional::ristReceiverThread()
{
    LOG(INFO, LOG_TAG) << "Starting RIST receiver thread for backchannel\n";

    while (running_) {
        if (!receiver_ctx_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        struct rist_data_block* data_block = nullptr;
        int queue_length = rist_receiver_data_read2(receiver_ctx_, &data_block, 100); // 100ms timeout

        if (queue_length > 0 && data_block) {
            try {
                LOG(DEBUG, LOG_TAG) << "Received backchannel data: " << data_block->payload_len 
                                   << " bytes on virtual port " << data_block->virt_dst_port << "\n";

                // Only process backchannel messages (client to server)
                if (data_block->virt_dst_port == VPORT_BACKCHANNEL) {
                    // Ensure we have enough buffer space
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    if (buffer_.size() < data_block->payload_len) {
                        buffer_.resize(data_block->payload_len);
                    }

                    // Copy data to our buffer
                    memcpy(buffer_.data(), data_block->payload, data_block->payload_len);

                    // Process the message - match TCP approach exactly 
                    if (data_block->payload_len >= base_msg_size_) {
                        try {
                            // Stage 1: Parse message header from a copy (don't modify original buffer)
                            std::vector<char> header_buffer(base_msg_size_);
                            memcpy(header_buffer.data(), buffer_.data(), base_msg_size_);
                            baseMessage_.deserialize(header_buffer.data());
                            
                            LOG(DEBUG, LOG_TAG) << "Parsed backchannel message header: type=" << baseMessage_.type 
                                               << ", size=" << baseMessage_.size << ", id=" << baseMessage_.id << "\n";
                            
                            if (baseMessage_.type > message_type::kLast) {
                                LOG(ERROR, LOG_TAG) << "Unknown backchannel message type received: " << baseMessage_.type << "\n";
                            }
                            else if (baseMessage_.size > msg::max_size) {
                                LOG(ERROR, LOG_TAG) << "Backchannel message too large: " << baseMessage_.size << "\n";
                            }
                            else if (baseMessage_.size <= data_block->payload_len) {
                                // Stage 2: Match TCP approach - create a buffer with the complete message
                                // TCP overwrites buffer with complete message, so we do the same
                                if (messageReceiver_ && self_) {
                                    LOG(DEBUG, LOG_TAG) << "Processing complete backchannel message from client\n";
                                    
                                    // Ensure buffer is exactly the message size (like TCP does)
                                    if (buffer_.size() != baseMessage_.size) {
                                        buffer_.resize(baseMessage_.size);
                                    }
                                    
                                    // Make sure we have exactly baseMessage_.size bytes
                                    // (buffer should already have the right data from the original copy)
                                    
                                    tv now;
                                    baseMessage_.received = now;
                                    // Follow WebSocket pattern: pass payload only (buffer + header_size)
                                    messageReceiver_->onMessageReceived(self_, baseMessage_, reinterpret_cast<char*>(buffer_.data()) + base_msg_size_);
                                } else {
                                    LOG(WARNING, LOG_TAG) << "Cannot process backchannel message - no message receiver or no self reference\n";
                                }
                            } else {
                                LOG(DEBUG, LOG_TAG) << "Incomplete backchannel message - expected " << baseMessage_.size << " bytes, got " << data_block->payload_len << "\n";
                            }
                        }
                        catch (const std::exception& e) {
                            LOG(ERROR, LOG_TAG) << "Error parsing backchannel message: " << e.what() << "\n";
                        }
                    } else {
                        LOG(DEBUG, LOG_TAG) << "Received data too small for Snapcast message header: " << data_block->payload_len << " < " << base_msg_size_ << "\n";
                    }
                }

                // Free the data block
                rist_receiver_data_block_free2(&data_block);
            }
            catch (const std::exception& e) {
                LOG(ERROR, LOG_TAG) << "Exception in RIST receiver thread: " << e.what() << "\n";
                if (data_block)
                    rist_receiver_data_block_free2(&data_block);
            }
        }
        else if (queue_length < 0) {
            LOG(ERROR, LOG_TAG) << "RIST receiver error: " << queue_length << "\n";
            break;
        }
    }

    LOG(INFO, LOG_TAG) << "RIST receiver thread stopped\n";
}

void StreamSessionRistBidirectional::senderConnectionStatusCallback(void* arg, struct rist_peer* /*peer*/, enum rist_connection_status status)
{
    auto* session = static_cast<StreamSessionRistBidirectional*>(arg);
    if (!session) {
        return;
    }

    switch (status) {
        case RIST_CONNECTION_ESTABLISHED:
            LOG(INFO, LOG_TAG) << "RIST sender connection established\n";
            session->sender_connected_ = true;
            session->connected_ = session->sender_connected_ && session->receiver_connected_;
            break;
        case RIST_CONNECTION_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST sender connection timed out\n";
            session->sender_connected_ = false;
            session->connected_ = false;
            if (session->messageReceiver_) {
                session->messageReceiver_->onDisconnect(session);
            }
            break;
        case RIST_CLIENT_CONNECTED:
            LOG(INFO, LOG_TAG) << "RIST sender client connected\n";
            session->sender_connected_ = true;
            session->connected_ = session->sender_connected_ && session->receiver_connected_;
            break;
        case RIST_CLIENT_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST sender client timed out\n";
            session->sender_connected_ = false;
            session->connected_ = false;
            if (session->messageReceiver_) {
                session->messageReceiver_->onDisconnect(session);
            }
            break;
        default:
            LOG(WARNING, LOG_TAG) << "Unknown RIST sender connection status: " << status << "\n";
            break;
    }
}

void StreamSessionRistBidirectional::receiverConnectionStatusCallback(void* arg, struct rist_peer* /*peer*/, enum rist_connection_status status)
{
    auto* session = static_cast<StreamSessionRistBidirectional*>(arg);
    if (!session) {
        return;
    }

    switch (status) {
        case RIST_CONNECTION_ESTABLISHED:
            LOG(INFO, LOG_TAG) << "RIST receiver connection established\n";
            session->receiver_connected_ = true;
            session->connected_ = session->sender_connected_ && session->receiver_connected_;
            break;
        case RIST_CONNECTION_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST receiver connection timed out\n";
            session->receiver_connected_ = false;
            session->connected_ = false;
            break;
        case RIST_CLIENT_CONNECTED:
            LOG(INFO, LOG_TAG) << "RIST receiver client connected\n";
            session->receiver_connected_ = true;
            session->connected_ = session->sender_connected_ && session->receiver_connected_;
            break;
        case RIST_CLIENT_TIMED_OUT:
            LOG(WARNING, LOG_TAG) << "RIST receiver client timed out\n";
            session->receiver_connected_ = false;
            session->connected_ = false;
            break;
        default:
            LOG(WARNING, LOG_TAG) << "Unknown RIST receiver connection status: " << status << "\n";
            break;
    }
}