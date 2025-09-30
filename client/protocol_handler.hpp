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
#include "common/message/wire_chunk.hpp"
#include "common/message/server_settings.hpp"
#include "common/message/codec_header.hpp"
#include "common/message/time.hpp"
#include "common/message/error.hpp"

// standard headers
#include <functional>
#include <memory>

namespace client
{

/**
 * Protocol Handler for Snapcast Client
 * 
 * Separates protocol message routing from application logic.
 * Handles incoming message dispatch and provides clean callback interface.
 */
class ProtocolHandler
{
public:
    // Callback types for different message handling

    /// Handler for wire chunk messages
    using WireChunkHandler = std::function<void(std::unique_ptr<msg::WireChunk>)>;
    /// Handler for server settings messages
    using ServerSettingsHandler = std::function<void(std::unique_ptr<msg::ServerSettings>)>;
    /// Handler for codec header messages
    using CodecHeaderHandler = std::function<void(std::unique_ptr<msg::CodecHeader>)>;
    /// Handler for time sync messages
    using TimeHandler = std::function<void(std::unique_ptr<msg::Time>)>;
    /// Handler for error messages
    using ErrorHandler = std::function<void(std::unique_ptr<msg::Error>)>;
    /// Handler for unexpected message types
    using UnexpectedMessageHandler = std::function<void(message_type type)>;

    ProtocolHandler() = default;
    ~ProtocolHandler() = default;

    // Callback registration methods

    /// Register handler for wire chunk messages
    void setWireChunkHandler(WireChunkHandler handler) { wire_chunk_handler_ = std::move(handler); }
    /// Register handler for server settings messages
    void setServerSettingsHandler(ServerSettingsHandler handler) { server_settings_handler_ = std::move(handler); }
    /// Register handler for codec header messages
    void setCodecHeaderHandler(CodecHeaderHandler handler) { codec_header_handler_ = std::move(handler); }
    /// Register handler for time sync messages
    void setTimeHandler(TimeHandler handler) { time_handler_ = std::move(handler); }
    /// Register handler for error messages
    void setErrorHandler(ErrorHandler handler) { error_handler_ = std::move(handler); }
    /// Register handler for unexpected message types
    void setUnexpectedMessageHandler(UnexpectedMessageHandler handler) { unexpected_message_handler_ = std::move(handler); }

    /**
     * Main message routing method
     * Dispatches incoming messages to appropriate handlers based on message type
     */
    void handleMessage(std::unique_ptr<msg::BaseMessage> message);

private:
    // Message type handlers
    WireChunkHandler wire_chunk_handler_;
    ServerSettingsHandler server_settings_handler_;
    CodecHeaderHandler codec_header_handler_;
    TimeHandler time_handler_;
    ErrorHandler error_handler_;
    UnexpectedMessageHandler unexpected_message_handler_;
};

} // namespace client
