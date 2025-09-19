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
#include "message/wire_block.hpp"
#include "message/wire_chunk.hpp"

// standard headers
#include <functional>
#include <memory>
#include <queue>


namespace wire_block
{

/// Server-side wire block accumulator
/**
 * Accumulates FLAC chunks into fixed-size wire blocks for zero-copy networking.
 * Handles fragmentation across multiple wire blocks when needed.
 */
class WireBlockAccumulator
{
public:
    /// Callback for completed wire blocks
    using WireBlockCallback = std::function<void(std::shared_ptr<msg::WireBlock>)>;
    
    /// c'tor with default wire block size
    explicit WireBlockAccumulator(WireBlockCallback callback) 
        : callback_(std::move(callback)), current_sequence_(0), wire_block_size_(msg::wire_block::DEFAULT_WIRE_BLOCK_SIZE)
    {
        current_block_ = std::make_shared<msg::WireBlock>(current_sequence_++, wire_block_size_);
    }
    
    /// c'tor with custom wire block size (from chunk_kb)
    WireBlockAccumulator(WireBlockCallback callback, size_t wire_block_size) 
        : callback_(std::move(callback)), current_sequence_(0), wire_block_size_(wire_block_size)
    {
        current_block_ = std::make_shared<msg::WireBlock>(current_sequence_++, wire_block_size_);
    }
    
    /// Add a FLAC chunk to be packed into wire blocks
    void addFlacChunk(const std::shared_ptr<msg::WireChunk>& flac_chunk)
    {
        if (!flac_chunk || flac_chunk->payloadSize == 0)
            return;
            
        const char* data = flac_chunk->payload;
        size_t remaining = flac_chunk->payloadSize;
        
        // Set timestamp from first chunk if current block is empty
        if (current_block_->isEmpty())
        {
            current_block_->timestamp = flac_chunk->timestamp;
        }
        
        // Pack data into wire blocks
        while (remaining > 0)
        {
            size_t added = current_block_->addData(data, remaining);
            data += added;
            remaining -= added;
            
            // If current block is full, send it and start a new one
            if (current_block_->isFull())
            {
                sendCurrentBlock();
                current_block_ = std::make_shared<msg::WireBlock>(current_sequence_++, wire_block_size_);
            }
        }
    }
    
    /// Force send the current block (even if not full)
    /// Used for chunk_kb mode to send exact-size blocks
    void flushCurrentBlock()
    {
        if (!current_block_->isEmpty())
        {
            sendCurrentBlock();
            current_block_ = std::make_shared<msg::WireBlock>(current_sequence_++, wire_block_size_);
        }
    }
    
    /// Get statistics
    struct Statistics
    {
        uint64_t blocks_sent = 0;
        uint64_t bytes_packed = 0;
        uint64_t flac_chunks_processed = 0;
        double average_fill_rate = 0.0;
    };
    
    const Statistics& getStatistics() const { return stats_; }

private:
    void sendCurrentBlock()
    {
        if (callback_ && !current_block_->isEmpty())
        {
            callback_(current_block_);
            
            // Update statistics
            stats_.blocks_sent++;
            stats_.bytes_packed += current_block_->payload_length;
            stats_.average_fill_rate = static_cast<double>(stats_.bytes_packed) / 
                                     (stats_.blocks_sent * current_block_->getMaxPayloadSize());
        }
    }
    
    WireBlockCallback callback_;
    std::shared_ptr<msg::WireBlock> current_block_;
    msg::wire_block::sequence_t current_sequence_;
    size_t wire_block_size_;
    Statistics stats_;
};


/// Client-side wire block reconstructor  
/**
 * Reconstructs complete FLAC chunks from received wire blocks.
 * Handles defragmentation when FLAC chunks span multiple wire blocks.
 */
class WireBlockReconstructor
{
public:
    /// Callback for reconstructed FLAC chunks
    using FlacChunkCallback = std::function<void(std::shared_ptr<msg::WireChunk>)>;
    
    /// c'tor
    explicit WireBlockReconstructor(FlacChunkCallback callback)
        : callback_(std::move(callback)), expected_sequence_(0)
    {
    }
    
    /// Process a received wire block
    void processWireBlock(std::shared_ptr<msg::WireBlock> wire_block)
    {
        if (!wire_block || wire_block->payload_length == 0)
            return;
            
        // Check sequence (for debugging - could add reordering later)
        if (wire_block->sequence_number != expected_sequence_)
        {
            // Log sequence mismatch but continue processing
            stats_.sequence_mismatches++;
        }
        expected_sequence_ = wire_block->sequence_number + 1;
        
        // Add data to reconstruction buffer
        reconstruction_buffer_.insert(reconstruction_buffer_.end(),
                                    wire_block->payload.begin(),
                                    wire_block->payload.begin() + wire_block->payload_length);
        
        // Try to extract complete FLAC chunks
        extractCompleteFlacChunks(wire_block->timestamp);
        
        stats_.blocks_received++;
        stats_.bytes_received += wire_block->payload_length;
    }
    
    /// Get statistics
    struct Statistics
    {
        uint64_t blocks_received = 0;
        uint64_t bytes_received = 0;
        uint64_t flac_chunks_reconstructed = 0;
        uint64_t sequence_mismatches = 0;
    };
    
    const Statistics& getStatistics() const { return stats_; }

private:
    void extractCompleteFlacChunks(const tv& timestamp)
    {
        // Simple approach: try to find complete FLAC chunks in buffer
        // This is a simplified version - real implementation would need
        // proper FLAC frame parsing to detect complete chunks
        
        while (reconstruction_buffer_.size() >= sizeof(uint32_t))
        {
            // Check if we have enough data for a chunk size header
            uint32_t chunk_size;
            std::memcpy(&chunk_size, reconstruction_buffer_.data(), sizeof(chunk_size));
            
            // Validate chunk size is reasonable
            if (chunk_size > 100000 || chunk_size == 0)
            {
                // Invalid chunk size, remove first byte and try again
                reconstruction_buffer_.erase(reconstruction_buffer_.begin());
                continue;
            }
            
            // Check if we have the complete chunk
            if (reconstruction_buffer_.size() >= chunk_size + sizeof(uint32_t))
            {
                // Create reconstructed FLAC chunk
                auto flac_chunk = std::make_shared<msg::WireChunk>(chunk_size);
                flac_chunk->timestamp = timestamp;
                std::memcpy(flac_chunk->payload, 
                           reconstruction_buffer_.data() + sizeof(uint32_t), 
                           chunk_size);
                
                // Remove processed data from buffer
                reconstruction_buffer_.erase(reconstruction_buffer_.begin(),
                                           reconstruction_buffer_.begin() + chunk_size + sizeof(uint32_t));
                
                // Send reconstructed chunk
                if (callback_)
                {
                    callback_(flac_chunk);
                    stats_.flac_chunks_reconstructed++;
                }
            }
            else
            {
                // Not enough data yet, wait for more wire blocks
                break;
            }
        }
    }
    
    FlacChunkCallback callback_;
    std::vector<char> reconstruction_buffer_;
    msg::wire_block::sequence_t expected_sequence_;
    Statistics stats_;
};

} // namespace wire_block