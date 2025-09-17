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

// standard headers
#include <cstddef>
#include <memory>

/// Forward declaration for polymorphic buffer guard
class BufferGuardBase;

/// Abstract base class for buffer pools
/**
 * Defines the common interface for all buffer pool implementations.
 * Allows polymorphic usage of different allocation strategies:
 * - DynamicBufferPool: malloc/free based with dynamic sizing
 * - MmapBufferPool: mmap/munmap based with page alignment for zero-copy
 */
class IBufferPool
{
public:
    virtual ~IBufferPool() = default;

    /// Acquire a buffer of at least the specified size
    virtual std::unique_ptr<BufferGuardBase> acquire(size_t size) = 0;

    /// Log buffer pool statistics
    virtual void logStats() const = 0;

    /// Get total number of buffers in pool
    virtual size_t getTotalBuffers() const = 0;

    /// Get number of available buffers
    virtual size_t getAvailableBuffers() const = 0;

    /// Get total bytes allocated by pool
    virtual size_t getTotalBytes() const = 0;

protected:
    /// Release a buffer back to the pool (called by BufferGuardBase destructor)
    virtual void releaseBuffer(void* buffer_data, size_t buffer_size) = 0;

    friend class BufferGuardBase;
};

/// Abstract base class for buffer guards with polymorphic pool access
/**
 * RAII wrapper for automatic buffer return to any buffer pool type.
 * Provides uniform interface regardless of underlying pool implementation.
 */
class BufferGuardBase
{
public:
    BufferGuardBase(IBufferPool* pool, void* data, size_t size)
        : pool_(pool), data_(data), size_(size)
    {
    }

    virtual ~BufferGuardBase()
    {
        if (pool_ && data_)
            pool_->releaseBuffer(data_, size_);
    }

    // No copying, only moving
    BufferGuardBase(const BufferGuardBase&) = delete;
    BufferGuardBase& operator=(const BufferGuardBase&) = delete;

    BufferGuardBase(BufferGuardBase&& other) noexcept
        : pool_(other.pool_), data_(other.data_), size_(other.size_)
    {
        other.pool_ = nullptr;
        other.data_ = nullptr;
        other.size_ = 0;
    }

    BufferGuardBase& operator=(BufferGuardBase&& other) noexcept
    {
        if (this != &other)
        {
            if (pool_ && data_)
                pool_->releaseBuffer(data_, size_);

            pool_ = other.pool_;
            data_ = other.data_;
            size_ = other.size_;

            other.pool_ = nullptr;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    /// Get raw buffer data pointer
    void* data() const { return data_; }

    /// Get buffer size
    size_t size() const { return size_; }

    /// Check if buffer is valid
    bool valid() const { return data_ != nullptr; }

protected:
    IBufferPool* pool_;
    void* data_;
    size_t size_;
};