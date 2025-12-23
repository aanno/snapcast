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
#include "zero_copy_pcm_chunk.hpp"

// local headers
#include "aixlog.hpp"

// standard headers
#include <cstring>

// static constexpr auto LOG_TAG = "ZeroCopyPcmChunk";

namespace msg
{

ZeroCopyPcmChunk::ZeroCopyPcmChunk(DynamicBufferPool::BufferGuard&& buffer_guard, const SampleFormat& format)
    : PcmChunk(), buffer_guard_(std::move(buffer_guard))
{
    this->format = format;
    initializeWithBufferPool();
}

ZeroCopyPcmChunk::ZeroCopyPcmChunk(DynamicBufferPool::BufferGuard&& buffer_guard, const SampleFormat& format, uint32_t estimated_ms)
    : PcmChunk(format, estimated_ms), buffer_guard_(std::move(buffer_guard))
{
    initializeWithBufferPool();
}

ZeroCopyPcmChunk::~ZeroCopyPcmChunk()
{
    // CRITICAL: Prevent WireChunk destructor from calling free() on buffer pool memory
    payload = nullptr;
    payloadSize = 0;
    // buffer_guard_ will automatically return the buffer to the pool via RAII
}

void ZeroCopyPcmChunk::initializeWithBufferPool()
{
    // Point payload directly to buffer pool memory
    payload = buffer_guard_.get().data();
    payloadSize = 0; // Start empty, will grow during decode

    // LOG(DEBUG, LOG_TAG) << "Created zero-copy PcmChunk with capacity: " << buffer_guard_.get().size() << " bytes\n";
}

bool ZeroCopyPcmChunk::ensureCapacity(size_t required_size)
{
    if (buffer_guard_.get().size() < required_size) {
        // Grow the buffer and update payload pointer
        buffer_guard_.resize(required_size);
        payload = buffer_guard_.get().data();
        return true;
    }
    return false;
}

// Factory functions
std::unique_ptr<ZeroCopyPcmChunk> createZeroCopyPcmChunk(size_t estimated_size, const SampleFormat& format)
{
    auto& buffer_pool = DynamicBufferPool::instance();
    auto buffer_guard = buffer_pool.acquire(estimated_size);

    return std::make_unique<ZeroCopyPcmChunk>(std::move(buffer_guard), format);
}

std::unique_ptr<ZeroCopyPcmChunk> createZeroCopyPcmChunk(const SampleFormat& format, uint32_t estimated_ms)
{
    auto& buffer_pool = DynamicBufferPool::instance();

    // Calculate estimated size based on format and duration
    size_t estimated_size = (format.rate() * estimated_ms / 1000) * format.frameSize();
    auto buffer_guard = buffer_pool.acquire(estimated_size);

    return std::make_unique<ZeroCopyPcmChunk>(std::move(buffer_guard), format, estimated_ms);
}

} // namespace msg
