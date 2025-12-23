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
#include "buffer_pool.hpp"
#include "message/pcm_chunk.hpp"

// standard headers
#include <memory>


namespace msg
{

/// Zero-copy PcmChunk that manages buffer pool memory directly
/**
 * This class extends PcmChunk to work directly with buffer pool memory,
 * eliminating the need for memory copies during audio processing.
 * The buffer pool memory is managed via RAII through BufferGuard.
 */
class ZeroCopyPcmChunk : public PcmChunk
{
public:
    /// Constructor taking ownership of buffer pool memory
    explicit ZeroCopyPcmChunk(DynamicBufferPool::BufferGuard&& buffer_guard, const SampleFormat& format = SampleFormat());

    /// Constructor with sample format and estimated size
    ZeroCopyPcmChunk(DynamicBufferPool::BufferGuard&& buffer_guard, const SampleFormat& format, uint32_t estimated_ms);

    /// Destructor - prevents free() on buffer pool memory
    ~ZeroCopyPcmChunk() override;

    /// Get the maximum capacity of the underlying buffer
    size_t getCapacity() const { return buffer_guard_.get().size(); }

    /// Resize the underlying buffer if needed (for dynamic growth)
    bool ensureCapacity(size_t required_size);

    /// Get direct access to the buffer guard (for advanced usage)
    const DynamicBufferPool::BufferGuard& getBufferGuard() const { return buffer_guard_; }

private:
    /// Buffer pool guard managing the memory lifetime
    DynamicBufferPool::BufferGuard buffer_guard_;

    /// Initialize the chunk with buffer pool memory
    void initializeWithBufferPool();
};

/// Factory function to create zero-copy PcmChunk
std::unique_ptr<ZeroCopyPcmChunk> createZeroCopyPcmChunk(size_t estimated_size, const SampleFormat& format = SampleFormat());

/// Factory function to create zero-copy PcmChunk with time-based sizing
std::unique_ptr<ZeroCopyPcmChunk> createZeroCopyPcmChunk(const SampleFormat& format, uint32_t estimated_ms);

} // namespace msg