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

// header include
#include "mmap_buffer_pool.hpp"

// system headers
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <stdexcept>

// Available buffer sizes (multiples of page size)
const std::vector<size_t> MmapBufferPool::SIZE_BUCKETS = {
    4096,   // 4K  - small messages
    8192,   // 8K  - medium messages
    16384,  // 16K - large messages
    32768,  // 32K - very large messages
    65536   // 64K - maximum size
};

// MmapBuffer implementation
MmapBufferPool::MmapBuffer::MmapBuffer(size_t buffer_size)
    : data(nullptr), size(buffer_size), last_used(std::chrono::steady_clock::now())
{
    // Allocate page-aligned memory using mmap
    data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) {
        throw std::runtime_error("Failed to allocate mmap buffer of size " + std::to_string(size));
    }
}

MmapBufferPool::MmapBuffer::~MmapBuffer()
{
    if (data && data != MAP_FAILED) {
        munmap(data, size);
        data = nullptr;
    }
}

MmapBufferPool::MmapBuffer::MmapBuffer(MmapBuffer&& other) noexcept
    : data(other.data), size(other.size), last_used(other.last_used)
{
    other.data = nullptr;
    other.size = 0;
}

MmapBufferPool::MmapBuffer& MmapBufferPool::MmapBuffer::operator=(MmapBuffer&& other) noexcept
{
    if (this != &other) {
        if (data && data != MAP_FAILED) {
            munmap(data, size);
        }
        data = other.data;
        size = other.size;
        last_used = other.last_used;
        other.data = nullptr;
        other.size = 0;
    }
    return *this;
}

// MmapBufferGuard implementation
MmapBufferPool::MmapBufferGuard::MmapBufferGuard(MmapBufferPool& pool, std::unique_ptr<MmapBuffer> buffer)
    : BufferGuardBase(&pool, buffer->data, buffer->size), buffer_(std::move(buffer))
{
}

// MmapBufferPool implementation
MmapBufferPool::MmapBufferPool(size_t initial_buffers_per_size)
    : initial_buffers_per_size_(initial_buffers_per_size), initialized_(false)
{
    // Pre-allocate initial buffers for each size bucket
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t bucket_size : SIZE_BUCKETS) {
        for (size_t i = 0; i < initial_buffers_per_size_; ++i) {
            available_buffers_[bucket_size].push_back(createBuffer(bucket_size));
        }
    }
    initialized_ = true;
}

MmapBufferPool::~MmapBufferPool()
{
    std::lock_guard<std::mutex> lock(mutex_);
    available_buffers_.clear(); // Will trigger MmapBuffer destructors
}

std::unique_ptr<BufferGuardBase> MmapBufferPool::acquire(size_t size)
{
    size_t bucket_size = findSizeBucket(size);

    std::lock_guard<std::mutex> lock(mutex_);

    auto& bucket = available_buffers_[bucket_size];
    std::unique_ptr<MmapBuffer> buffer;

    if (!bucket.empty()) {
        buffer = std::move(bucket.front());
        bucket.pop_front();
        buffers_reused_++;
    } else {
        buffer = createBuffer(bucket_size);
        buffers_created_++;
    }

    return std::make_unique<MmapBufferGuard>(*this, std::move(buffer));
}

void MmapBufferPool::release(std::unique_ptr<MmapBuffer> buffer)
{
    if (!buffer) return;

    buffer->last_used = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    available_buffers_[buffer->size].push_back(std::move(buffer));
}

void MmapBufferPool::releaseBuffer(void* buffer_data, size_t buffer_size)
{
    // This is called by BufferGuardBase destructor
    // The actual buffer is managed by MmapBufferGuard destructor
}

size_t MmapBufferPool::findSizeBucket(size_t size) const
{
    // Find smallest bucket that fits the requested size
    auto it = std::lower_bound(SIZE_BUCKETS.begin(), SIZE_BUCKETS.end(), size);
    if (it != SIZE_BUCKETS.end()) {
        return *it;
    }
    // If size is larger than largest bucket, use largest bucket
    return SIZE_BUCKETS.back();
}

std::unique_ptr<MmapBufferPool::MmapBuffer> MmapBufferPool::createBuffer(size_t size)
{
    auto buffer = std::make_unique<MmapBuffer>(size);
    total_buffers_++;
    total_bytes_ += size;
    return buffer;
}

void MmapBufferPool::logStats() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    size_t available_count = 0;
    for (const auto& bucket : available_buffers_) {
        available_count += bucket.second.size();
    }

    LOG(INFO, "MmapBufferPool") << "=== Mmap Buffer Pool Stats ===\n";
    LOG(INFO, "MmapBufferPool") << "\tTotal Buffers: " << total_buffers_.load() << "\n";
    LOG(INFO, "MmapBufferPool") << "\tAvailable Buffers: " << available_count << "\n";
    LOG(INFO, "MmapBufferPool") << "\tBytes Allocated: " << total_bytes_.load() << "\n";
    LOG(INFO, "MmapBufferPool") << "\tBuffers Created: " << buffers_created_.load() << "\n";
    LOG(INFO, "MmapBufferPool") << "\tBuffers Reused: " << buffers_reused_.load() << "\n";
    LOG(INFO, "MmapBufferPool") << "\tPage Size: " << getPageSize() << "\n";
}

size_t MmapBufferPool::getTotalBuffers() const
{
    return total_buffers_.load();
}

size_t MmapBufferPool::getAvailableBuffers() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    size_t available_count = 0;
    for (const auto& bucket : available_buffers_) {
        available_count += bucket.second.size();
    }
    return available_count;
}

size_t MmapBufferPool::getTotalBytes() const
{
    return total_bytes_.load();
}

size_t MmapBufferPool::getPageSize()
{
    static size_t page_size = sysconf(_SC_PAGESIZE);
    return page_size > 0 ? page_size : DEFAULT_PAGE_SIZE;
}

size_t MmapBufferPool::roundToPageSize(size_t size)
{
    size_t page_size = getPageSize();
    return ((size + page_size - 1) / page_size) * page_size;
}

bool MmapBufferPool::isPageAligned(void* addr)
{
    size_t page_size = getPageSize();
    return (reinterpret_cast<uintptr_t>(addr) % page_size) == 0;
}