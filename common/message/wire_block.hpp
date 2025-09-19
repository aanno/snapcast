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
#include "message.hpp"
#include "wire_chunk.hpp"

// standard headers
#include <cstdint>
#include <vector>


namespace msg
{

/// Wire Block Protocol Constants
namespace wire_block
{
    /// Size of BaseMessage header (type + id + refersTo + sent + received + size)
    /// 3 * uint16_t + 2 * tv + uint32_t = 6 + 16 + 4 = 26 bytes
    constexpr size_t BASE_MESSAGE_HEADER_SIZE = 26;
    
    /// Size of WireBlock specific header (timestamp + sequence_number + payload_length)
    /// tv + uint32_t + uint32_t = 8 + 4 + 4 = 16 bytes  
    constexpr size_t WIRE_BLOCK_HEADER_SIZE = 16;
    
    /// Total header size for wire block messages
    constexpr size_t TOTAL_HEADER_SIZE = BASE_MESSAGE_HEADER_SIZE + WIRE_BLOCK_HEADER_SIZE;
    
    /// Default wire block size for zero-copy networking (4KB)
    constexpr size_t DEFAULT_WIRE_BLOCK_SIZE = 4096;
    
    /// Calculate payload size for given wire block size
    constexpr size_t getPayloadSize(size_t wire_block_size) {
        return wire_block_size - TOTAL_HEADER_SIZE;
    }
    
    /// Default available payload size in each wire block
    constexpr size_t DEFAULT_WIRE_BLOCK_PAYLOAD_SIZE = getPayloadSize(DEFAULT_WIRE_BLOCK_SIZE);
    
    /// Wire block sequence number type
    using sequence_t = uint32_t;
}

/// Fixed-size wire block for zero-copy networking
/**
 * Wire blocks are fixed-size containers that carry FLAC chunk data
 * in fixed 4KB blocks for optimal zero-copy performance.
 * 
 * A wire block can contain:
 * - Complete FLAC chunk(s)
 * - Partial FLAC chunk (fragmented across multiple blocks)
 * - End of one FLAC chunk + beginning of next
 */
class WireBlock : public BaseMessage
{
public:
    /// c'tor with default size
    WireBlock() : BaseMessage(message_type::kWireChunk), sequence_number(0), payload_length(0), 
                 wire_block_size_(wire_block::DEFAULT_WIRE_BLOCK_SIZE)
    {
        payload.resize(wire_block::getPayloadSize(wire_block_size_), 0);
    }
    
    /// c'tor with sequence number and default size
    explicit WireBlock(wire_block::sequence_t seq) : BaseMessage(message_type::kWireChunk), sequence_number(seq), payload_length(0),
                                                    wire_block_size_(wire_block::DEFAULT_WIRE_BLOCK_SIZE)
    {
        payload.resize(wire_block::getPayloadSize(wire_block_size_), 0);
    }
    
    /// c'tor with sequence number and custom wire block size
    WireBlock(wire_block::sequence_t seq, size_t wire_block_size) : BaseMessage(message_type::kWireChunk), sequence_number(seq), payload_length(0),
                                                                   wire_block_size_(wire_block_size)
    {
        payload.resize(wire_block::getPayloadSize(wire_block_size_), 0);
    }
    
    void read(std::istream& stream) override
    {
        readVal(stream, timestamp.sec);
        readVal(stream, timestamp.usec);
        readVal(stream, sequence_number);
        readVal(stream, payload_length);
        
        // Read payload data
        if (payload_length > 0 && payload_length <= getMaxPayloadSize())
        {
            stream.read(payload.data(), payload_length);
        }
    }
    
    uint32_t getSize() const override
    {
        return wire_block::TOTAL_HEADER_SIZE + payload_length;
    }
    
    /// Get available space in this wire block
    size_t getAvailableSpace() const
    {
        return wire_block::getPayloadSize(wire_block_size_) - payload_length;
    }
    
    /// Get maximum payload size for this wire block
    size_t getMaxPayloadSize() const
    {
        return wire_block::getPayloadSize(wire_block_size_);
    }
    
    /// Get wire block size
    size_t getWireBlockSize() const
    {
        return wire_block_size_;
    }
    
    /// Add data to this wire block
    /// @return number of bytes actually added
    size_t addData(const char* data, size_t size)
    {
        size_t available = getAvailableSpace();
        size_t to_add = std::min(size, available);
        
        if (to_add > 0)
        {
            std::memcpy(payload.data() + payload_length, data, to_add);
            payload_length += to_add;
        }
        
        return to_add;
    }
    
    /// Check if this wire block is full
    bool isFull() const
    {
        return payload_length >= wire_block::getPayloadSize(wire_block_size_);
    }
    
    /// Check if this wire block is empty
    bool isEmpty() const
    {
        return payload_length == 0;
    }

    tv timestamp;                                    ///< playout timestamp (server time)
    wire_block::sequence_t sequence_number;          ///< wire block sequence number
    uint32_t payload_length;                         ///< actual payload length
    std::vector<char> payload;                       ///< configurable-size payload buffer

private:
    size_t wire_block_size_;                         ///< total wire block size (including headers)

protected:
    void doserialize(std::ostream& stream) const override
    {
        writeVal(stream, timestamp.sec);
        writeVal(stream, timestamp.usec);
        writeVal(stream, sequence_number);
        writeVal(stream, payload_length);
        
        // Write payload data
        if (payload_length > 0)
        {
            stream.write(payload.data(), payload_length);
        }
    }
};

} // namespace msg