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
    LOG(INFO, LOG_TAG) << "Initializing RIST logging\n";

    log_settings_ = {};
    log_settings_.log_level = RIST_LOG_DEBUG; // Set debug level
    log_settings_.log_stream = nullptr; // stdout; // Output to stdout
    log_settings_.log_cb = rist_log_callback; // Set callback
    log_settings_.log_cb_arg = const_cast<char*>(LOG_GLOBAL);
    rist_logging_set_global(&log_settings_);

    LOG(INFO, LOG_TAG) << "Initializing bidirectional RIST server\n";
    
    // Create RIST sender context for sending audio/control to clients
    rist_logging_settings log_settings_sender = log_settings_;
    log_settings_sender.log_cb_arg = const_cast<char*>(LOG_SENDER);
    int ret = rist_sender_create(&sender_ctx_, RIST_PROFILE_MAIN, 0, &log_settings_sender);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST sender context: " << ret << "\n";
        return false;
    }

    // Create RIST receiver context for receiving backchannel from clients
    rist_logging_settings log_settings_receiver = log_settings_;
    log_settings_receiver.log_cb_arg = const_cast<char*>(LOG_RECEIVER);
    ret = rist_receiver_create(&receiver_ctx_, RIST_PROFILE_MAIN, &log_settings_receiver);
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

    // Set data callback for event-driven reception (replaces polling)
    ret = rist_receiver_data_callback_set2(receiver_ctx_, ristDataCallback, this);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to set receiver data callback: " << ret << "\n";
        cleanupRist();
        return false;
    }
    
    // Set stats callback to monitor packet flow (debugging aid)
    ret = rist_stats_callback_set(receiver_ctx_, 1000, StreamSessionRistBidirectional::ristStatsCallback, this);
    if (ret != 0) {
        LOG(WARNING, LOG_TAG) << "Failed to set RIST receiver stats callback: " << ret << "\n";
    } else {
        LOG(DEBUG, LOG_TAG) << "RIST receiver stats callback set successfully\n";
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
    
    // Manually set optimized RIST parameters for low latency audio streaming
    sender_config->recovery_length_min = recovery_length_min;
    sender_config->recovery_length_max = recovery_length_max;
    sender_config->recovery_rtt_min = recovery_rtt_min;
    sender_config->recovery_rtt_max = recovery_rtt_max;
    sender_config->recovery_reorder_buffer = recovery_reorder_buffer;
    sender_config->min_retries = min_retries;
    sender_config->max_retries = max_retries;
    sender_config->congestion_control_mode = congestion_control_mode;
    
    LOG(INFO, LOG_TAG) << "Applied optimized RIST sender parameters: bufmin=" << sender_config->recovery_length_min 
                       << "ms, bufmax=" << sender_config->recovery_length_max << "ms, rttmin=" << sender_config->recovery_rtt_min 
                       << "ms, rttmax=" << sender_config->recovery_rtt_max << "ms\n";

    ret = rist_peer_create(sender_ctx_, &sender_peer_, sender_config);
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST sender peer: " << ret << "\n";
        free(sender_config);
        cleanupRist();
        return false;
    }
    free(sender_config);

    // Configure receiver context to bind on a different port for backchannel
    uint16_t backchannel_port = client_port_ + 2;  // Use +2 to avoid RTCP conflict
    std::string receiver_url = "rist://@0.0.0.0:" + std::to_string(backchannel_port);
    LOG(INFO, LOG_TAG) << "Using RIST receiver URL (bind): " << receiver_url << "\n";

    struct rist_peer_config* receiver_config = nullptr;
    ret = rist_parse_address2(receiver_url.c_str(), &receiver_config);
    if (ret < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to parse RIST receiver URL: " << receiver_url << ", error: " << ret << "\n";
        cleanupRist();
        return false;
    }
    
    // Manually set optimized RIST parameters for low latency backchannel communication
    receiver_config->recovery_length_min = recovery_length_min;
    receiver_config->recovery_length_max = recovery_length_max;
    receiver_config->recovery_rtt_min = recovery_rtt_min;
    receiver_config->recovery_rtt_max = recovery_rtt_max;
    receiver_config->recovery_reorder_buffer = recovery_reorder_buffer;
    receiver_config->min_retries = min_retries;
    receiver_config->max_retries = max_retries;
    receiver_config->congestion_control_mode = congestion_control_mode;
    LOG(INFO, LOG_TAG) << "Applied optimized RIST receiver parameters: bufmin=" << receiver_config->recovery_length_min 
                       << "ms, bufmax=" << receiver_config->recovery_length_max << "ms, rttmin=" << receiver_config->recovery_rtt_min 
                       << "ms, rttmax=" << receiver_config->recovery_rtt_max << "ms\n";

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
    
    // Initialize self_ for message processing (required for server Hello/ServerSettings/CodecHeader flow)
    self_ = std::static_pointer_cast<StreamSessionRistBidirectional>(shared_from_this());
    
    // Start worker thread for processing messages from callback
    worker_thread_ = std::thread(&StreamSessionRistBidirectional::messageProcessorThread, this);
    
    // Data reception is handled by callback + worker thread
    LOG(INFO, LOG_TAG) << "RIST data callback and worker thread active\n";
    
    readNext();
}

void StreamSessionRistBidirectional::stop()
{
    LOG(DEBUG, LOG_TAG) << "Stopping bidirectional RIST session\n";
    
    running_ = false;
    connected_ = false;
    sender_connected_ = false;
    receiver_connected_ = false;
    
    // Wake up worker thread and wait for it to finish
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
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
    if (buffer.begin() != buffer.end() && boost::asio::buffer_size(*buffer.begin()) >= sizeof(msg::BaseMessage)) {
        auto* data = boost::asio::buffer_cast<const char*>(*buffer.begin());
        msg::BaseMessage base_msg;
        base_msg.deserialize(const_cast<char*>(data));
        
        LOG(DEBUG, LOG_TAG) << "Message type: " << base_msg.type << ", size: " << base_msg.size << "\n";
        
        // Explicit message type to virtual port mapping
        if (base_msg.type == message_type::kServerSettings) {
            LOG(DEBUG, LOG_TAG) << "ServerSettings -> VPORT_CONTROL (" << VPORT_CONTROL << ")\n";
            return VPORT_CONTROL; // ServerSettings on 2000
        } else if (base_msg.type == message_type::kCodecHeader) {
            LOG(INFO, LOG_TAG) << "*** TRACE CODECHEADER *** Server sending CodecHeader (" << base_msg.size << " bytes) -> VPORT_AUDIO (" << VPORT_AUDIO << ")\n";
            return VPORT_AUDIO; // CodecHeader on 1000
        } else if (base_msg.type == message_type::kWireChunk) {
            return VPORT_AUDIO; // Audio data on 1000
        }
        
        // Fallback to size-based heuristic for other message types
        size_t data_size = boost::asio::buffer_size(*buffer.begin());
        if (data_size > 1024) {
            LOG(DEBUG, LOG_TAG) << "Large packet (" << data_size << " bytes) -> VPORT_AUDIO (" << VPORT_AUDIO << ")\n";
            return VPORT_AUDIO;  // Large packets are likely audio
        } else {
            LOG(DEBUG, LOG_TAG) << "Small packet (" << data_size << " bytes) -> VPORT_CONTROL (" << VPORT_CONTROL << ")\n";
            return VPORT_CONTROL; // Small packets are likely control messages
        }
    }
    
    LOG(DEBUG, LOG_TAG) << "Default -> VPORT_CONTROL (" << VPORT_CONTROL << ")\n";
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

    // Add special tracing for CodecHeader
    if (virt_port == VPORT_AUDIO && data_size > 1000) {
        // Likely CodecHeader - add enhanced tracing
        if (data_size >= sizeof(msg::BaseMessage)) {
            msg::BaseMessage temp_msg;
            temp_msg.deserialize(const_cast<char*>(data_ptr));
            if (temp_msg.type == message_type::kCodecHeader) {
                LOG(INFO, LOG_TAG) << "*** TRACE CODECHEADER *** Server about to send CodecHeader via RIST: " << data_size << " bytes on vport " << virt_port << "\n";
            }
        }
    }
    
    // Send data via RIST sender context
    int ret = rist_sender_data_write(sender_ctx_, &data_block);
    if (ret < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to send data via RIST: " << ret << "\n";
        if (handler) {
            handler(boost::system::error_code(boost::asio::error::broken_pipe), 0);
        }
        return;
    }

    // Enhanced logging for CodecHeader
    if (virt_port == VPORT_AUDIO && data_size > 1000) {
        if (data_size >= sizeof(msg::BaseMessage)) {
            msg::BaseMessage temp_msg;
            temp_msg.deserialize(const_cast<char*>(data_ptr));
            if (temp_msg.type == message_type::kCodecHeader) {
                LOG(INFO, LOG_TAG) << "*** TRACE CODECHEADER *** Server successfully sent CodecHeader via RIST: " << data_size << " bytes (ret=" << ret << ")\n";
            } else {
                LOG(DEBUG, LOG_TAG) << "Sent " << data_size << " bytes via RIST on virtual port " << virt_port << " (ret=" << ret << ")\n";
            }
        } else {
            LOG(DEBUG, LOG_TAG) << "Sent " << data_size << " bytes via RIST on virtual port " << virt_port << " (ret=" << ret << ")\n";
        }
    } else {
        LOG(DEBUG, LOG_TAG) << "Sent " << data_size << " bytes via RIST on virtual port " << virt_port << " (ret=" << ret << ")\n";
    }
    
    if (handler) {
        handler(boost::system::error_code(), data_size);
    }
}

int StreamSessionRistBidirectional::ristDataCallback(void* arg, struct rist_data_block* data_block)
{
    auto* session = static_cast<StreamSessionRistBidirectional*>(arg);
    if (!session || !data_block) {
        LOG(ERROR, LOG_TAG) << "Invalid callback args or data block\n";
        return 0;
    }
    
    LOG(DEBUG, LOG_TAG) << "Data callback triggered: " << data_block->payload_len 
                       << " bytes on vport " << data_block->virt_dst_port << "\n";

    try {
        // Only queue backchannel messages (client to server)
        if (data_block->virt_dst_port == VPORT_BACKCHANNEL) {
            // Minimal processing in callback - just queue the data
            QueuedMessage msg;
            msg.data.resize(data_block->payload_len);
            memcpy(msg.data.data(), data_block->payload, data_block->payload_len);
            msg.virt_port = data_block->virt_dst_port;
            
            {
                std::lock_guard<std::mutex> lock(session->queue_mutex_);
                session->message_queue_.push(std::move(msg));
            }
            session->queue_cv_.notify_one();
            
            LOG(DEBUG, LOG_TAG) << "Queued backchannel data: " << data_block->payload_len 
                               << " bytes on virtual port " << data_block->virt_dst_port << "\n";
        }
    }
    catch (const std::exception& e) {
        LOG(ERROR, LOG_TAG) << "Exception in RIST data callback: " << e.what() << "\n";
    }

    return 0; // Success
}

int StreamSessionRistBidirectional::ristStatsCallback(void* arg, const struct rist_stats* stats)
{
    auto* session = static_cast<StreamSessionRistBidirectional*>(arg);
    if (!session || !stats) {
        return 0;
    }
    
    if (stats->stats_json) {
        LOG(DEBUG, LOG_TAG) << "RIST receiver stats: " << stats->stats_json << "\n";
    } else {
        LOG(DEBUG, LOG_TAG) << "RIST receiver stats callback triggered (no JSON data)\n";
    }
    
    return 0;
}

void StreamSessionRistBidirectional::messageProcessorThread()
{
    LOG(INFO, LOG_TAG) << "Starting RIST message processor thread\n";

    while (running_) {
        QueuedMessage msg;
        
        // Wait for messages
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !message_queue_.empty() || !running_; });
            
            if (!running_) break;
            
            if (message_queue_.empty()) continue;
            
            msg = std::move(message_queue_.front());
            message_queue_.pop();
        }
        
        try {
            // Process the message (heavy processing moved out of callback)
            if (msg.virt_port == VPORT_BACKCHANNEL) {
                // Ensure we have enough buffer space
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                if (buffer_.size() < msg.data.size()) {
                    buffer_.resize(msg.data.size());
                }

                // Copy data to our buffer
                memcpy(buffer_.data(), msg.data.data(), msg.data.size());

                // Process the message - match TCP approach exactly 
                if (msg.data.size() >= base_msg_size_) {
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
                        else if (baseMessage_.size <= msg.data.size()) {
                            // Stage 2: Match TCP approach - create a buffer with the complete message
                            if (messageReceiver_ && self_) {
                                LOG(DEBUG, LOG_TAG) << "Processing complete backchannel message from client\n";
                                
                                // Ensure buffer is exactly the message size (like TCP does)
                                if (buffer_.size() != baseMessage_.size) {
                                    buffer_.resize(baseMessage_.size);
                                }
                                
                                tv now;
                                baseMessage_.received = now;
                                // RIST pattern: pass payload only (server expects JSON payload, not full buffer)
                                messageReceiver_->onMessageReceived(self_, baseMessage_, reinterpret_cast<char*>(buffer_.data()) + base_msg_size_);
                            } else {
                                LOG(WARNING, LOG_TAG) << "Cannot process backchannel message - no message receiver or no self reference\n";
                            }
                        } else {
                            LOG(DEBUG, LOG_TAG) << "Incomplete backchannel message - expected " << baseMessage_.size << " bytes, got " << msg.data.size() << "\n";
                        }
                    }
                    catch (const std::exception& e) {
                        LOG(ERROR, LOG_TAG) << "Error parsing backchannel message: " << e.what() << "\n";
                    }
                } else {
                    LOG(DEBUG, LOG_TAG) << "Received data too small for Snapcast message header: " << msg.data.size() << " < " << base_msg_size_ << "\n";
                }
            }
        }
        catch (const std::exception& e) {
            LOG(ERROR, LOG_TAG) << "Exception in message processor thread: " << e.what() << "\n";
        }
    }
    
    LOG(INFO, LOG_TAG) << "RIST message processor thread stopped\n";
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
