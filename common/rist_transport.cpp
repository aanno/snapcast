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
#include "rist_transport.hpp"

// local headers
#include "aixlog.hpp"
#include "message/factory.hpp"
#include "message/hello.hpp"
#include "message/server_settings.hpp"
#include "message/codec_header.hpp"

// standard headers
#include <sstream>
#include <cstdlib>

using namespace std;

// RIST logging callback removed - causes crashes in libRIST


RistTransport::RistTransport(Mode mode, RistTransportReceiver* receiver)
    : mode_(mode), receiver_(receiver), sender_ctx_(nullptr), receiver_ctx_(nullptr), port_(0), running_(false)
{
}

RistTransport::~RistTransport()
{
    stop();
}

bool RistTransport::configureServer(const std::string& bind_address, uint16_t port)
{
    if (mode_ != Mode::SERVER) {
        LOG(ERROR, LOG_TAG) << "Cannot configure server on client mode transport\n";
        return false;
    }
    address_ = bind_address;
    port_ = port;
    return true;
}

bool RistTransport::configureClient(const std::string& server_address, uint16_t port)
{
    if (mode_ != Mode::CLIENT) {
        LOG(ERROR, LOG_TAG) << "Cannot configure client on server mode transport\n";
        return false;
    }
    address_ = server_address;
    port_ = port;
    return true;
}

bool RistTransport::start()
{
    if (running_)
        return true;

    if (address_.empty() || port_ == 0) {
        LOG(ERROR, LOG_TAG) << "Transport not configured - call configureServer() or configureClient() first\n";
        return false;
    }

    LOG(INFO, LOG_TAG) << "Starting RIST transport in " << (mode_ == Mode::SERVER ? "SERVER" : "CLIENT") << " mode\n";
    LOG(INFO, LOG_TAG) << "Virtual ports: " << VPORT_AUDIO << " (audio), " << VPORT_CONTROL << " (control), " << VPORT_BACKCHANNEL << " (backchannel)\n";

    // Skip RIST logging setup - it causes crashes in libRIST
    LOG(INFO, LOG_TAG) << "Skipping RIST logging setup to avoid libRIST crashes\n";

    // Create RIST contexts
    if (rist_sender_create(&sender_ctx_, RIST_PROFILE_MAIN, 0, nullptr) != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST sender context\n";
        return false;
    }

    if (rist_receiver_create(&receiver_ctx_, RIST_PROFILE_MAIN, nullptr) != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST receiver context\n";
        rist_destroy(sender_ctx_);
        sender_ctx_ = nullptr;
        return false;
    }

    // Configure peers based on mode (following testrist pattern)
    if (mode_ == Mode::SERVER) {
        // Server: bind to ports (like testrist server)
        string sender_url = "rist://@" + address_ + ":" + to_string(port_);        // Port 1706 for sending to clients
        string receiver_url = "rist://@" + address_ + ":" + to_string(port_ + 2);  // Port 1708 for receiving from clients
        
        if (!createPeer(sender_ctx_, sender_url, "sender") || !createPeer(receiver_ctx_, receiver_url, "receiver")) {
            stop();
            return false;
        }
    } 
    else {
        // Client: connect to server (like testrist client)
        string receiver_url = "rist://" + address_ + ":" + to_string(port_);       // Connect to server port 1706 for receiving
        string sender_url = "rist://" + address_ + ":" + to_string(port_ + 2);     // Connect to server port 1708 for sending
        
        if (!createPeer(receiver_ctx_, receiver_url, "receiver") || !createPeer(sender_ctx_, sender_url, "sender")) {
            stop();
            return false;
        }
    }

    // Set data callback for receiving backchannel messages
    if (rist_receiver_data_callback_set2(receiver_ctx_, dataCallback, this) != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to set RIST data callback\n";
        stop();
        return false;
    }

    // Start RIST contexts
    if (rist_start(sender_ctx_) != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to start RIST sender\n";
        stop();
        return false;
    }

    if (rist_start(receiver_ctx_) != 0)
    {
        LOG(ERROR, LOG_TAG) << "Failed to start RIST receiver\n";
        stop();
        return false;
    }

    running_ = true;
    LOG(INFO, LOG_TAG) << "RIST transport started successfully\n";
    LOG(INFO, LOG_TAG) << "Virtual ports: " << VPORT_AUDIO << " (audio), " << VPORT_CONTROL << " (control), " << VPORT_BACKCHANNEL << " (backchannel)\n";
    
    return true;
}

void RistTransport::stop()
{
    if (!running_)
        return;
        
    running_ = false;
    connected_clients_.clear();

    if (sender_ctx_)
    {
        rist_destroy(sender_ctx_);
        sender_ctx_ = nullptr;
    }

    if (receiver_ctx_)
    {
        rist_destroy(receiver_ctx_);
        receiver_ctx_ = nullptr;
    }

    LOG(INFO, LOG_TAG) << "RIST transport stopped\n";
}

bool RistTransport::createPeer(struct rist_ctx* ctx, const std::string& url, const std::string& type)
{
    LOG(INFO, LOG_TAG) << "Creating RIST " << type << " peer: " << url << "\n";
    
    struct rist_peer_config* config = nullptr;
    if (rist_parse_address2(url.c_str(), &config) != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to parse RIST URL: " << url << "\n";
        return false;
    }

    // Apply optimized parameters like testrist
    config->recovery_length_min = 200;  // 200ms buffer
    config->recovery_length_max = 200;
    config->recovery_rtt_min = 5;
    config->recovery_rtt_max = 500;
    config->recovery_reorder_buffer = 15;
    config->min_retries = 6;
    config->max_retries = 20;

    struct rist_peer* peer;
    int ret = rist_peer_create(ctx, &peer, config);
    
    // Free the config structure after use (libRIST copies what it needs)
    free(config);
    
    if (ret != 0) {
        LOG(ERROR, LOG_TAG) << "Failed to create RIST " << type << " peer for: " << url << "\n";
        return false;
    }

    LOG(INFO, LOG_TAG) << "Successfully created RIST " << type << " peer\n";
    return true;
}

bool RistTransport::sendAudioChunk(const std::shared_ptr<msg::PcmChunk>& chunk)
{
    return sendMessage(VPORT_AUDIO, *chunk);
}

bool RistTransport::sendMessage(uint16_t vport, const msg::BaseMessage& message)
{
    if (!running_ || !sender_ctx_) {
        return false;
    }

    // Serialize message
    ostringstream oss;
    message.serialize(oss);
    string serialized = oss.str();

    // Create RIST data block
    struct rist_data_block data_block = {};
    data_block.payload = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(serialized.data()));
    data_block.payload_len = serialized.size();
    data_block.virt_dst_port = vport;
    data_block.ts_ntp = 0; // Let RIST handle timestamps

    int ret = rist_sender_data_write(sender_ctx_, &data_block);
    if (ret < 0) {
        LOG(DEBUG, LOG_TAG) << "Failed to send message type " << message.type << " on vport " << vport << ", ret=" << ret << "\n";
        return false;
    }
    if (message.type != message_type::kWireChunk) // avoid log spam for audio chunks
        LOG(DEBUG, LOG_TAG) << "Sent message type " << message.type << " (" << serialized.size() << " bytes) on vport " << vport << "\n";
    return true;
}

bool RistTransport::sendRawData(uint16_t vport, const void* data, size_t size)
{
    if (!running_ || !sender_ctx_) {
        LOG(WARNING, LOG_TAG) << "Cannot send raw data - transport not available or not running\n";
        return false;
    }
    
    if (!data || size == 0) {
        LOG(WARNING, LOG_TAG) << "Cannot send empty data\n";
        return false;
    }
    
    // Create RIST data block
    struct rist_data_block data_block = {};
    data_block.payload = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(data));
    data_block.payload_len = size;
    data_block.virt_dst_port = vport;
    data_block.ts_ntp = 0; // Let RIST handle timestamps
    
    int ret = rist_sender_data_write(sender_ctx_, &data_block);
    if (ret < 0) {
        LOG(ERROR, LOG_TAG) << "Failed to send raw data (" << size << " bytes) on vport " << vport << ", ret=" << ret << "\n";
        return false;
    }
    
    LOG(DEBUG, LOG_TAG) << "Sent raw data (" << size << " bytes) on vport " << vport << "\n";
    return true;
}

int RistTransport::dataCallback(void* arg, struct rist_data_block* data_block)
{
    if (!arg) {
        return 0;
    }
    
    auto* transport = static_cast<RistTransport*>(arg);
    return transport->handleDataCallback(data_block);
}

int RistTransport::handleDataCallback(struct rist_data_block* data_block)
{
    if (!data_block || !data_block->payload || data_block->payload_len == 0)
        return 0;

    LOG(DEBUG, LOG_TAG) << "Received " << data_block->payload_len << " bytes on vport " << data_block->virt_dst_port << "\n";

    try {
        // Parse message header
        if (data_block->payload_len < msg::BaseMessage().getSize()) {
            LOG(WARNING, LOG_TAG) << "Received message too small: " << data_block->payload_len << " bytes\n";
            return 0;
        }

        msg::BaseMessage baseMessage;
        baseMessage.deserialize(const_cast<char*>(reinterpret_cast<const char*>(data_block->payload)));
        
        LOG(DEBUG, LOG_TAG) << "Received message type: " << baseMessage.type << ", size: " << baseMessage.size << " on vport " << data_block->virt_dst_port << "\n";

        // Extract payload (everything after the base message header)
        string payload;
        if (data_block->payload_len > baseMessage.getSize()) {
            const char* payload_start = reinterpret_cast<const char*>(data_block->payload) + baseMessage.getSize();
            size_t payload_size = data_block->payload_len - baseMessage.getSize();
            payload.assign(payload_start, payload_size);
        }

        // Handle specific message types for server mode
        if (mode_ == Mode::SERVER && baseMessage.type == message_type::kHello) {
            // Parse Hello to get clientId 
            if (!payload.empty()) {
                msg::Hello hello;
                hello.deserialize(baseMessage, const_cast<char*>(payload.data()));
                string clientId = hello.getUniqueId();
                
                LOG(INFO, LOG_TAG) << "RIST Hello received from client: " << clientId << "\n";
                connected_clients_[clientId] = true;
                
                // Notify receiver about new client connection
                if (receiver_) {
                    receiver_->onRistClientConnected(clientId);
                }
            }
        }

        // Forward all messages to receiver
        if (receiver_) {
            receiver_->onRistMessageReceived(baseMessage, payload, data_block->virt_dst_port);
        }
    }
    catch (const exception& e) {
        LOG(ERROR, LOG_TAG) << "Error processing RIST message: " << e.what() << "\n";
    }

    return 0;
}

#endif // HAS_LIBRIST
