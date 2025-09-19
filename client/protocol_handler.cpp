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
#include "protocol_handler.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/message/factory.hpp"

// standard headers
#include <memory>

using namespace std;

static constexpr auto LOG_TAG = "ProtocolHandler";

namespace client
{

void ProtocolHandler::handleMessage(std::unique_ptr<msg::BaseMessage> message)
{
    if (!message)
    {
        LOG(WARNING, LOG_TAG) << "Received null message\n";
        return;
    }

    switch (message->type)
    {
        case message_type::kWireChunk:
        {
            // Try to cast as WireBlock first (factory will have created the correct type)
            auto wire_block = msg::message_cast<msg::WireBlock>(std::move(message));
            if (wire_block && wire_block_handler_)
            {
                LOG(TRACE, LOG_TAG) << "Routing WireBlock message, sequence: " << wire_block->sequence_number 
                                   << ", payload: " << wire_block->payload_length << " bytes\n";
                wire_block_handler_(std::move(wire_block));
            }
            else
            {
                // Fall back to WireChunk/PcmChunk handling
                LOG(TRACE, LOG_TAG) << "Routing WireChunk message\n";
                if (wire_chunk_handler_)
                {
                    auto wire_chunk = msg::message_cast<msg::WireChunk>(std::move(message));
                    wire_chunk_handler_(std::move(wire_chunk));
                }
                else
                {
                    LOG(WARNING, LOG_TAG) << "No WireChunk handler registered\n";
                }
            }
            break;
        }
        
        case message_type::kServerSettings:
        {
            LOG(DEBUG, LOG_TAG) << "Routing ServerSettings message\n";
            if (server_settings_handler_)
            {
                auto server_settings = msg::message_cast<msg::ServerSettings>(std::move(message));
                server_settings_handler_(std::move(server_settings));
            }
            else
            {
                LOG(WARNING, LOG_TAG) << "No ServerSettings handler registered\n";
            }
            break;
        }
        
        case message_type::kCodecHeader:
        {
            LOG(DEBUG, LOG_TAG) << "Routing CodecHeader message\n";
            if (codec_header_handler_)
            {
                auto codec_header = msg::message_cast<msg::CodecHeader>(std::move(message));
                codec_header_handler_(std::move(codec_header));
            }
            else
            {
                LOG(WARNING, LOG_TAG) << "No CodecHeader handler registered\n";
            }
            break;
        }
        
        case message_type::kTime:
        {
            LOG(TRACE, LOG_TAG) << "Routing Time message\n";
            if (time_handler_)
            {
                auto time_message = msg::message_cast<msg::Time>(std::move(message));
                time_handler_(std::move(time_message));
            }
            else
            {
                LOG(WARNING, LOG_TAG) << "No Time handler registered\n";
            }
            break;
        }
        
        case message_type::kError:
        {
            LOG(DEBUG, LOG_TAG) << "Routing Error message\n";
            if (error_handler_)
            {
                auto error_message = msg::message_cast<msg::Error>(std::move(message));
                error_handler_(std::move(error_message));
            }
            else
            {
                LOG(WARNING, LOG_TAG) << "No Error handler registered\n";
            }
            break;
        }
        
        default:
        {
            LOG(WARNING, LOG_TAG) << "Unexpected message type: " << message->type << "\n";
            if (unexpected_message_handler_)
            {
                unexpected_message_handler_(message->type);
            }
            break;
        }
    }
}

} // namespace client
