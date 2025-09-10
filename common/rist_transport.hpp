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

#ifdef HAS_LIBRIST

// local headers
#include "aixlog.hpp"
#include "message/message.hpp"
#include "message/hello.hpp"
#include "message/server_settings.hpp"  
#include "message/codec_header.hpp"
#include "message/pcm_chunk.hpp"

// 3rd party headers
#include <librist/librist.h>

// standard headers
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

/// Forward declarations
namespace msg {
    class BaseMessage;
    class PcmChunk;
}

/// RIST transport callback interface
class RistTransportReceiver
{
public:
    virtual ~RistTransportReceiver() = default;
    
    /// Called when a message is received via RIST
    virtual void onRistMessageReceived(const msg::BaseMessage& baseMessage, const std::string& payload, uint16_t vport) = 0;
    /// Called when a client connects (server side only)
    virtual void onRistClientConnected(const std::string& clientId) = 0;
    /// Called when a client disconnects (server side only)  
    virtual void onRistClientDisconnected(const std::string& clientId) = 0;
};

/// Bidirectional RIST transport using virtual port multiplexing (testrist model)
/// Can be used by both server and client
class RistTransport
{
public:
    /// Virtual ports following testrist model
    static constexpr uint16_t VPORT_AUDIO = 1000;
    static constexpr uint16_t VPORT_CONTROL = 2000; 
    static constexpr uint16_t VPORT_BACKCHANNEL = 3000;

    /// Transport mode
    enum class Mode {
        SERVER,  ///< Server mode (bind and accept connections)
        CLIENT   ///< Client mode (connect to server)
    };

    /// c'tor
    RistTransport(Mode mode, RistTransportReceiver* receiver = nullptr);
    /// d'tor
    virtual ~RistTransport();

    /// Configure as server (bind to address/port)
    bool configureServer(const std::string& bind_address, uint16_t port);
    /// Configure as client (connect to server address/port)  
    bool configureClient(const std::string& server_address, uint16_t port);

    /// Start RIST transport
    bool start();
    /// Stop RIST transport
    void stop();

    /// Send message on specified virtual port
    bool sendMessage(uint16_t vport, const msg::BaseMessage& message);
    /// Send raw data directly on specified virtual port (for pre-serialized data)
    bool sendRawData(uint16_t vport, const void* data, size_t size);
    /// Send audio chunk (convenience method for VPORT_AUDIO)
    bool sendAudioChunk(const std::shared_ptr<msg::PcmChunk>& chunk);

private:
    /// RIST data callback (static for C API)
    static int dataCallback(void* arg, struct rist_data_block* data_block);
    /// Instance data callback handler
    int handleDataCallback(struct rist_data_block* data_block);
    /// Helper to create and configure RIST peer
    bool createPeer(struct rist_ctx* ctx, const std::string& url, const std::string& type);

    Mode mode_;
    RistTransportReceiver* receiver_;
    
    struct rist_ctx* sender_ctx_;
    struct rist_ctx* receiver_ctx_;
    
    std::string address_;
    uint16_t port_;
    
    /// Track connected clients by clientId (server mode only)
    std::unordered_map<std::string, bool> connected_clients_;
    
    bool running_;
    
public:
    static constexpr auto LOG_TAG = "RistTransport";
};

#endif // HAS_LIBRIST