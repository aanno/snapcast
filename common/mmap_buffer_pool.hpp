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
#include "buffer_pool_base.hpp"

// standard headers
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

/// Page-aligned mmap buffer pool for true zero-copy TCP receive
/**
 * Thread-safe buffer pool using mmap for page-aligned allocations.
 * Designed specifically for TCP_ZEROCOPY_RECEIVE which requires:
 * - Page-aligned buffer addresses
 * - Buffer sizes that are multiples of page size
 * - Direct kernel mapping capability via socket file descriptor
 *
 * Features:
 * - mmap/munmap based allocation for zero-copy compatibility
 * - Fixed page-size buckets (4K, 8K, 16K, 32K, 64K)
 * - Thread-safe operations for multi-client scenarios
 * - RAII buffer management with automatic return
 * - Statistics tracking for zero-copy performance analysis
 */
class MmapBufferPool : public IBufferPool
{
public:
    /// Page-aligned buffer with mmap allocation
    struct MmapBuffer
    {
        void* data;                    ///< mmap'd memory address (page-aligned)
        size_t size;                   ///< Buffer size (multiple of page size)
        std::chrono::steady_clock::time_point last_used; ///< Last usage timestamp
        int socket_fd;                 ///< Socket file descriptor for zero-copy

        explicit MmapBuffer(size_t buffer_size, int socket_fd);
        ~MmapBuffer();

        // No copying, only moving
        MmapBuffer(const MmapBuffer&) = delete;
        MmapBuffer& operator=(const MmapBuffer&) = delete;
        MmapBuffer(MmapBuffer&& other) noexcept;
        MmapBuffer& operator=(MmapBuffer&& other) noexcept;
    };

    /// RAII wrapper for mmap buffers
    class MmapBufferGuard : public BufferGuardBase
    {
    public:
        MmapBufferGuard(MmapBufferPool& pool, std::unique_ptr<MmapBuffer> buffer);

        /// Get typed buffer data
        template<typename T = char>
        T* data() const { return static_cast<T*>(BufferGuardBase::data()); }

        /// Resize not supported for mmap buffers (fixed page sizes)
        void resize(size_t /*new_size*/) = delete;

    private:
        std::unique_ptr<MmapBuffer> buffer_;
    };

public:
    /// Constructor
    MmapBufferPool(size_t initial_buffers_per_size, int socket_fd);

    /// Destructor
    ~MmapBufferPool() override;

    // IBufferPool interface
    std::unique_ptr<BufferGuardBase> acquire(size_t size) override;
    void logStats() const override;
    size_t getTotalBuffers() const override;
    size_t getAvailableBuffers() const override;
    size_t getTotalBytes() const override;

    /// Get system page size
    static size_t getPageSize();

    /// Round up size to next page boundary
    static size_t roundToPageSize(size_t size);

    /// Check if address is page-aligned
    static bool isPageAligned(void* addr);

private:
    void releaseBuffer(void* buffer_data, size_t buffer_size) override;

    /// Release an mmap buffer back to the pool
    void release(std::unique_ptr<MmapBuffer> buffer);

    /// Find appropriate size bucket for requested size
    size_t findSizeBucket(size_t size) const;

    /// Create new buffer of specified size
    std::unique_ptr<MmapBuffer> createBuffer(size_t size);

private:
    static constexpr size_t DEFAULT_PAGE_SIZE = 4096;
    static const std::vector<size_t> SIZE_BUCKETS; ///< Available buffer sizes (multiples of page size)

    mutable std::mutex mutex_;
    std::map<size_t, std::deque<std::unique_ptr<MmapBuffer>>> available_buffers_; ///< Available buffers by size

    // Statistics (atomic for thread-safety)
    mutable std::atomic<size_t> total_buffers_{0};
    mutable std::atomic<size_t> buffers_created_{0};
    mutable std::atomic<size_t> buffers_reused_{0};
    mutable std::atomic<size_t> total_bytes_{0};
    mutable std::atomic<size_t> cleanup_operations_{0};

    int socket_fd_; ///< Socket file descriptor for zero-copy
    size_t initial_buffers_per_size_;
    bool initialized_;

    friend class MmapBufferGuard;
};
