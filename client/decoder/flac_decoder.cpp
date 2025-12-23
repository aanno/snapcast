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
#include "flac_decoder.hpp"

// local headers
#include "common/aixlog.hpp"
#include "common/endian.hpp"
#include "common/snap_exception.hpp"

// standard headers
#include <cstring>
#include <iomanip>
#include <iostream>


using namespace std;

static constexpr auto LOG_TAG = "FlacDecoder";

namespace decoder
{

namespace callback
{
// NOLINTBEGIN
FLAC__StreamDecoderReadStatus read_callback(const FLAC__StreamDecoder* decoder, FLAC__byte buffer[], size_t* bytes, void* client_data);
FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder* decoder, const FLAC__Frame* frame, const FLAC__int32* const buffer[],
                                              void* client_data);
void metadata_callback(const FLAC__StreamDecoder* decoder, const FLAC__StreamMetadata* metadata, void* client_data);
void error_callback(const FLAC__StreamDecoder* decoder, FLAC__StreamDecoderErrorStatus status, void* client_data);
// NOLINTEND
} // namespace callback

// Global variables removed - now using instance members in FlacDecoder class


FlacDecoder::FlacDecoder() : Decoder(), lastError_(nullptr), flac_chunk_(std::make_unique<msg::PcmChunk>()),
                             buffer_pool_(DynamicBufferPool::instance()),
                             output_buffer_guard_(buffer_pool_.acquire(8192))  // Initial 8KB buffer from pool
{
}


FlacDecoder::~FlacDecoder()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (decoder_)
        FLAC__stream_decoder_delete(decoder_);
    // flac_chunk_ is automatically cleaned up by unique_ptr
}


bool FlacDecoder::decode(msg::PcmChunk* chunk)
{
    std::lock_guard<std::mutex> lock(mutex_);
    decode_operations_++;
    cacheInfo_.reset();
    pcm_chunk_ = chunk;
    
    // TRUE ZERO-COPY: No input buffer copy - read directly from original payload
    // Point flac_chunk directly to original payload (NO COPY!)
    flac_chunk_->payload = chunk->payload;
    flac_chunk_->payloadSize = chunk->payloadSize;
    input_read_pos_ = 0;  // Reset read position for new decode

    // Reset output buffer to 0 size (this will use realloc to 0, essentially freeing it)
    pcm_chunk_->payload = static_cast<char*>(realloc(pcm_chunk_->payload, 0)); // NOLINT
    pcm_chunk_->payloadSize = 0;

    // TRUE ZERO-COPY: Use buffer pool for output instead of direct allocation
    size_t estimated_output_size = chunk->payloadSize * 2; // Estimate 2x expansion for typical FLAC
    output_buffer_guard_ = buffer_pool_.acquire(estimated_output_size);
    output_capacity_ = output_buffer_guard_.get().size();
    output_bytes_used_ = 0; // Track how much of our buffer we've used
    while (flac_chunk_->payloadSize > 0)
    {
        if (FLAC__stream_decoder_process_single(decoder_) == 0)
        {
            return false;
        }

        if (lastError_)
        {
            LOG(ERROR, LOG_TAG) << "FLAC decode error: " << FLAC__StreamDecoderErrorStatusString[*lastError_] << "\n";
            lastError_ = nullptr;
            return false;
        }
    }

    if ((cacheInfo_.cachedBlocks_ > 0) && (cacheInfo_.sampleRate_ != 0))
    {
        double diffMs = static_cast<double>(cacheInfo_.cachedBlocks_) / (static_cast<double>(cacheInfo_.sampleRate_) / 1000.);
        auto us = static_cast<uint64_t>(diffMs * 1000.);
        tv diff(static_cast<int32_t>(us / 1000000), static_cast<int32_t>(us % 1000000));
        LOG(TRACE, LOG_TAG) << "Cached: " << cacheInfo_.cachedBlocks_ << ", " << diffMs << "ms, " << diff.sec << "s, " << diff.usec << "us\n";
        chunk->timestamp = chunk->timestamp - diff;
    }

    // Copy accumulated data from buffer pool to PcmChunk
    if (output_bytes_used_ > 0) {
        pcm_chunk_->payload = static_cast<char*>(realloc(pcm_chunk_->payload, output_bytes_used_));
        memcpy(pcm_chunk_->payload, output_buffer_guard_.get().data(), output_bytes_used_);
        pcm_chunk_->payloadSize = static_cast<uint32_t>(output_bytes_used_);
    }
    // Buffer guard automatically returns buffer to pool when it goes out of scope

    // Log growth statistics periodically
    logGrowthStatistics();

    return true;
}

std::unique_ptr<msg::ZeroCopyPcmChunk> FlacDecoder::decodeZeroCopy(msg::PcmChunk* chunk)
{
    std::lock_guard<std::mutex> lock(mutex_);
    decode_operations_++;
    zero_copy_operations_++;
    cacheInfo_.reset();

    // TRUE ZERO-COPY: No input buffer copy - read directly from original payload
    // Point flac_chunk directly to original payload (NO COPY!)
    flac_chunk_->payload = chunk->payload;
    flac_chunk_->payloadSize = chunk->payloadSize;
    input_read_pos_ = 0;  // Reset read position for new decode

    // Create zero-copy output chunk with estimated size (4x expansion for safety)
    size_t estimated_output_size = chunk->payloadSize * 4;
    zero_copy_chunk_ = msg::createZeroCopyPcmChunk(estimated_output_size, sample_format_);

    // LOG(DEBUG, LOG_TAG) << "Phase 4 TRUE Zero-Copy decode started - input: " << chunk->payloadSize
    //                    << " bytes, estimated output: " << estimated_output_size << " bytes\n";

    // Set up for zero-copy callbacks to write directly to the ZeroCopyPcmChunk
    pcm_chunk_ = zero_copy_chunk_.get();  // Callbacks will write to this
    output_bytes_used_ = 0; // Reset for new decode

    // Process FLAC data (callbacks will write directly to zero_copy_chunk_)
    while (flac_chunk_->payloadSize > 0)
    {
        if (FLAC__stream_decoder_process_single(decoder_) == 0)
        {
            return nullptr; // Decode failed
        }

        if (lastError_)
        {
            LOG(ERROR, LOG_TAG) << "FLAC decode error: " << FLAC__StreamDecoderErrorStatusString[*lastError_] << "\\n";
            lastError_ = nullptr;
            return nullptr;
        }
    }

    // Handle timing adjustments (same as regular decode)
    if ((cacheInfo_.cachedBlocks_ > 0) && (cacheInfo_.sampleRate_ != 0))
    {
        double diffMs = static_cast<double>(cacheInfo_.cachedBlocks_) / (static_cast<double>(cacheInfo_.sampleRate_) / 1000.);
        auto us = static_cast<uint64_t>(diffMs * 1000.);
        tv diff(static_cast<int32_t>(us / 1000000), static_cast<int32_t>(us % 1000000));
        LOG(TRACE, LOG_TAG) << "Cached: " << cacheInfo_.cachedBlocks_ << ", " << diffMs << "ms, " << diff.sec << "s, " << diff.usec << "us\n";
        zero_copy_chunk_->timestamp = chunk->timestamp - diff;
    } else {
        zero_copy_chunk_->timestamp = chunk->timestamp;
    }

    // NO COPY NEEDED! The zero_copy_chunk_ already contains the decoded data
    // Set final payload size based on what was actually written
    zero_copy_chunk_->payloadSize = static_cast<uint32_t>(output_bytes_used_);

    // Log growth statistics periodically
    logGrowthStatistics();

    // Return ownership of the zero-copy chunk
    return std::move(zero_copy_chunk_);
}


SampleFormat FlacDecoder::setHeader(msg::CodecHeader* chunk)
{
    flac_header_ = chunk;
    FLAC__StreamDecoderInitStatus init_status;

    if ((decoder_ = FLAC__stream_decoder_new()) == nullptr)
        throw SnapException("ERROR: allocating decoder");

    //	(void)FLAC__stream_decoder_set_md5_checking(decoder_, true);
    init_status = FLAC__stream_decoder_init_stream(decoder_, callback::read_callback, nullptr, nullptr, nullptr, nullptr, callback::write_callback,
                                                   callback::metadata_callback, callback::error_callback, this);
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        throw SnapException("ERROR: initializing decoder: " + string(FLAC__StreamDecoderInitStatusString[init_status]));

    FLAC__stream_decoder_process_until_end_of_metadata(decoder_);
    if (sample_format_.rate() == 0)
        throw SnapException("Sample format not found");

    return sample_format_;
}

void FlacDecoder::logGrowthStatistics() const
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_stats_log_);

    if (elapsed.count() >= 30) { // Log every 30 seconds
        uint64_t expansions = buffer_expansions_.load();
        uint64_t operations = decode_operations_.load();
        uint64_t zero_copy_ops = zero_copy_operations_.load();
        uint64_t regular_ops = operations - zero_copy_ops;

        double expansion_rate = operations > 0 ? (double(expansions) / double(operations)) * 100.0 : 0.0;
        double zero_copy_rate = operations > 0 ? (double(zero_copy_ops) / double(operations)) * 100.0 : 0.0;

        LOG(INFO, LOG_TAG) << "=== FLAC Decoder Buffer Growth Stats (every 30s) ===\n"
                          << "Decode Operations: " << operations << ", "
                          << "Buffer Expansions: " << expansions << ", "
                          << "Expansion Rate: " << std::fixed << std::setprecision(2) << expansion_rate << "%\n"
                          << "Zero-Copy Operations: " << zero_copy_ops << ", "
                          << "Regular Operations: " << regular_ops << ", "
                          << "Zero-Copy Rate: " << std::fixed << std::setprecision(2) << zero_copy_rate << "%\n"
                          << "Current Capacity: " << output_capacity_ << " bytes\n";

        last_stats_log_ = now;
    }
}

namespace callback
{
// NOLINTNEXTLINE
FLAC__StreamDecoderReadStatus read_callback(const FLAC__StreamDecoder* /*decoder*/, FLAC__byte buffer[], size_t* bytes, void* client_data)
{
    auto* flac_decoder = static_cast<FlacDecoder*>(client_data);
    
    if (flac_decoder->flac_header_ != nullptr)
    {
        *bytes = flac_decoder->flac_header_->payloadSize;
        memcpy(buffer, flac_decoder->flac_header_->payload, *bytes);
        flac_decoder->flac_header_ = nullptr;
    }
    else if (flac_decoder->flac_chunk_ != nullptr)
    {
        //		cerr << "read_callback: " << *bytes << ", avail: " << flac_decoder->flac_chunk_->payloadSize << "\n";
        flac_decoder->cacheInfo_.isCachedChunk_ = false;
        
        // Calculate remaining bytes from current read position
        size_t remaining = flac_decoder->flac_chunk_->payloadSize - flac_decoder->input_read_pos_;
        if (*bytes > remaining)
            *bytes = remaining;

        //		if (*bytes == 0)
        //			return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;

        // Copy from current read position (NO memmove needed!)
        memcpy(buffer, flac_decoder->flac_chunk_->payload + flac_decoder->input_read_pos_, *bytes);
        
        // Advance read position instead of shifting data
        flac_decoder->input_read_pos_ += *bytes;
        
        // Update remaining size for compatibility
        flac_decoder->flac_chunk_->payloadSize = flac_decoder->flac_chunk_->payloadSize - static_cast<uint32_t>(*bytes);
    }
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

// NOLINTNEXTLINE
FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder* /*decoder*/, const FLAC__Frame* frame, const FLAC__int32* const buffer[],
                                              void* client_data)
{
    auto* flacDecoder = static_cast<FlacDecoder*>(client_data);
    
    if (flacDecoder->pcm_chunk_ != nullptr)
    {
        size_t bytes = frame->header.blocksize * flacDecoder->sample_format_.frameSize();

        if (flacDecoder->cacheInfo_.isCachedChunk_)
            flacDecoder->cacheInfo_.cachedBlocks_ += frame->header.blocksize;

        // Handle buffer growth for both regular and zero-copy modes
        size_t required_size = flacDecoder->output_bytes_used_ + bytes;

        // Check if we're in zero-copy mode
        if (auto* zc_chunk = dynamic_cast<msg::ZeroCopyPcmChunk*>(flacDecoder->pcm_chunk_)) {
            // Zero-copy mode: resize the ZeroCopyPcmChunk buffer if needed
            if (zc_chunk->ensureCapacity(required_size)) {
                flacDecoder->buffer_expansions_++;
            }
        } else {
            // Regular mode: use buffer pool approach
            if (required_size > flacDecoder->output_capacity_) {
                // Get larger buffer from pool with some headroom to avoid frequent reallocations
                size_t new_capacity = required_size + (required_size / 2); // 1.5x growth
                flacDecoder->output_buffer_guard_ = flacDecoder->buffer_pool_.acquire(new_capacity);
                flacDecoder->output_capacity_ = flacDecoder->output_buffer_guard_.get().size();
                flacDecoder->buffer_expansions_++;
            }
        }

        for (size_t channel = 0; channel < flacDecoder->sample_format_.channels(); ++channel)
        {
            if (buffer[channel] == nullptr)
            {
                LOG(ERROR, LOG_TAG) << "ERROR: buffer[" << channel << "] is NULL\n";
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }

            // Write to the appropriate buffer based on mode
            char* output_ptr;
            if (auto* zc_chunk = dynamic_cast<msg::ZeroCopyPcmChunk*>(flacDecoder->pcm_chunk_)) {
                // Zero-copy mode: write directly to ZeroCopyPcmChunk payload
                output_ptr = zc_chunk->payload + flacDecoder->output_bytes_used_;
            } else {
                // Regular mode: write to buffer pool buffer
                output_ptr = flacDecoder->output_buffer_guard_.get().data() + flacDecoder->output_bytes_used_;
            }

            if (flacDecoder->sample_format_.sampleSize() == 1)
            {
                auto* chunkBuffer = reinterpret_cast<int8_t*>(output_ptr);
                for (size_t i = 0; i < frame->header.blocksize; i++)
                    chunkBuffer[flacDecoder->sample_format_.channels() * i + channel] = static_cast<int8_t>(buffer[channel][i]);
            }
            else if (flacDecoder->sample_format_.sampleSize() == 2)
            {
                auto* chunkBuffer = reinterpret_cast<int16_t*>(output_ptr);
                for (size_t i = 0; i < frame->header.blocksize; i++)
                    chunkBuffer[flacDecoder->sample_format_.channels() * i + channel] = SWAP_16((int16_t)(buffer[channel][i]));
            }
            else if (flacDecoder->sample_format_.sampleSize() == 4)
            {
                auto* chunkBuffer = reinterpret_cast<int32_t*>(output_ptr);
                for (size_t i = 0; i < frame->header.blocksize; i++)
                    chunkBuffer[flacDecoder->sample_format_.channels() * i + channel] = SWAP_32((int32_t)(buffer[channel][i]));
            }
        }
        flacDecoder->output_bytes_used_ += bytes;
    }

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}


void metadata_callback(const FLAC__StreamDecoder* /*decoder*/, const FLAC__StreamMetadata* metadata, void* client_data)
{
    auto* flacDecoder = static_cast<FlacDecoder*>(client_data);
    
    /* print some stats */
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO)
    {
        flacDecoder->cacheInfo_.sampleRate_ = metadata->data.stream_info.sample_rate;
        flacDecoder->sample_format_.setFormat(metadata->data.stream_info.sample_rate, static_cast<uint16_t>(metadata->data.stream_info.bits_per_sample),
                               static_cast<uint16_t>(metadata->data.stream_info.channels));
    }
}


void error_callback(const FLAC__StreamDecoder* /*decoder*/, FLAC__StreamDecoderErrorStatus status, void* client_data)
{
    LOG(ERROR, LOG_TAG) << "Got error callback: " << FLAC__StreamDecoderErrorStatusString[status] << "\n";
    static_cast<FlacDecoder*>(client_data)->lastError_ = std::make_unique<FLAC__StreamDecoderErrorStatus>(status);
}
} // namespace callback

} // namespace decoder
