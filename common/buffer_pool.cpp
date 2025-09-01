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
#include "buffer_pool.hpp"

// local headers
#include "aixlog.hpp"

// standard headers
#include <algorithm>
#include <map>

static constexpr auto LOG_TAG = "BufferPool";

DynamicBufferPool::DynamicBufferPool(size_t initial_count, size_t default_buffer_size)
    : default_buffer_size_(std::max(default_buffer_size, MIN_BUFFER_SIZE))
    , last_cleanup_(std::chrono::steady_clock::now())
{
    // Pre-allocate initial buffers
    std::lock_guard<std::mutex> lock(mutex_);
    size_t size_bucket = get_size_bucket(default_buffer_size_);
    
    for (size_t i = 0; i < initial_count; ++i)
    {
        auto buffer = create_buffer(default_buffer_size_);
        available_buffers_[size_bucket].push(std::move(buffer));
    }
    
    LOG(DEBUG, LOG_TAG) << "Initialized buffer pool with " << initial_count 
                        << " buffers of size " << default_buffer_size_ << "\\n";
}

DynamicBufferPool::BufferGuard DynamicBufferPool::acquire(size_t min_size)
{
    size_t target_size = std::max({min_size, default_buffer_size_, MIN_BUFFER_SIZE});
    size_t size_bucket = get_size_bucket(target_size);
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Try to reuse an existing buffer from the same or larger size bucket
    auto it = available_buffers_.lower_bound(size_bucket);
    while (it != available_buffers_.end())
    {
        if (!it->second.empty())
        {
            auto buffer = std::move(const_cast<std::stack<std::unique_ptr<Buffer>>&>(it->second).top());
            const_cast<std::stack<std::unique_ptr<Buffer>>&>(it->second).pop();
            
            buffer->resize_if_needed(target_size);
            buffers_reused_++;
            
            LOG(TRACE, LOG_TAG) << "Reused buffer from size bucket " << it->first 
                                << " for requested size " << target_size << "\\n";
            
            return BufferGuard(*this, std::move(buffer));
        }
        ++it;
    }
    
    // No suitable buffer found, create a new one
    auto buffer = create_buffer(target_size);
    buffers_created_++;
    
    LOG(TRACE, LOG_TAG) << "Created new buffer of size " << target_size << "\\n";
    
    return BufferGuard(*this, std::move(buffer));
}

void DynamicBufferPool::release(std::unique_ptr<Buffer> buffer)
{
    if (!buffer)
        return;
        
    size_t size_bucket = get_size_bucket(buffer->capacity);
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if we have room in this size bucket
    if (available_buffers_[size_bucket].size() < MAX_POOL_SIZE)
    {
        buffer->last_used = std::chrono::steady_clock::now();
        available_buffers_[size_bucket].push(std::move(buffer));
        
        LOG(TRACE, LOG_TAG) << "Returned buffer to pool, size bucket " << size_bucket << "\\n";
    }
    else
    {
        // Pool is full for this size, let buffer be destroyed
        total_buffers_--;
        bytes_allocated_ -= buffer->capacity;
        
        LOG(TRACE, LOG_TAG) << "Pool full for size bucket " << size_bucket 
                            << ", destroying buffer\\n";
    }
}

std::unique_ptr<DynamicBufferPool::Buffer> DynamicBufferPool::create_buffer(size_t size)
{
    auto buffer = std::make_unique<Buffer>(size);
    total_buffers_++;
    bytes_allocated_ += size;
    return buffer;
}

size_t DynamicBufferPool::get_size_bucket(size_t size)
{
    // Round up to next power of 2 for bucketing
    size_t bucket = MIN_BUFFER_SIZE;
    while (bucket < size)
        bucket *= GROWTH_FACTOR;
    return bucket;
}

DynamicBufferPool::Stats DynamicBufferPool::getStats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    Stats stats;
    stats.total_buffers = total_buffers_.load();
    stats.bytes_allocated = bytes_allocated_.load();
    stats.buffers_created = buffers_created_.load();
    stats.buffers_reused = buffers_reused_.load();
    stats.cleanup_operations = cleanup_operations_.load();
    
    // Count available buffers
    for (const auto& bucket : available_buffers_)
    {
        stats.available_buffers += bucket.second.size();
    }
    
    return stats;
}

void DynamicBufferPool::resetStats()
{
    buffers_created_ = 0;
    buffers_reused_ = 0;
    cleanup_operations_ = 0;
}

void DynamicBufferPool::cleanup(std::chrono::seconds max_idle_time)
{
    auto now = std::chrono::steady_clock::now();
    
    // Don't cleanup too frequently
    if (now - last_cleanup_ < std::chrono::seconds(30))
        return;
        
    std::lock_guard<std::mutex> lock(mutex_);
    last_cleanup_ = now;
    cleanup_operations_++;
    
    size_t cleaned_count = 0;
    
    for (auto& bucket_pair : available_buffers_)
    {
        auto& stack = bucket_pair.second;
        std::stack<std::unique_ptr<Buffer>> temp_stack;
        
        // Check each buffer in the stack
        while (!stack.empty())
        {
            auto buffer = std::move(const_cast<std::stack<std::unique_ptr<Buffer>>&>(stack).top());
            const_cast<std::stack<std::unique_ptr<Buffer>>&>(stack).pop();
            
            if (now - buffer->last_used < max_idle_time)
            {
                // Buffer is still fresh, keep it
                temp_stack.push(std::move(buffer));
            }
            else
            {
                // Buffer is stale, let it be destroyed
                total_buffers_--;
                bytes_allocated_ -= buffer->capacity;
                cleaned_count++;
            }
        }
        
        // Move fresh buffers back
        stack = std::move(temp_stack);
    }
    
    if (cleaned_count > 0)
    {
        LOG(DEBUG, LOG_TAG) << "Cleaned up " << cleaned_count << " stale buffers\\n";
    }
}

DynamicBufferPool& DynamicBufferPool::instance()
{
    static DynamicBufferPool instance_;
    return instance_;
}