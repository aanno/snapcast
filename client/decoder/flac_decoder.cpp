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
                             input_buffer_guard_(buffer_pool_.acquire(4096)),  // Initial size
                             output_buffer_guard_(buffer_pool_.acquire(8192))  // Initial size
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
    cacheInfo_.reset();
    pcm_chunk_ = chunk;
    
    // Use buffer pool for input buffer instead of realloc
    if (input_buffer_guard_.get().size() < chunk->payloadSize) {
        input_buffer_guard_.resize(chunk->payloadSize);
    }
    auto& input_buffer = input_buffer_guard_.get();
    
    // Copy input data to buffer pool buffer (still needed because FLAC modifies it)
    memcpy(input_buffer.data(), chunk->payload, chunk->payloadSize);
    
    // Point flac_chunk to our buffer pool buffer and reset read position
    flac_chunk_->payload = input_buffer.data();
    flac_chunk_->payloadSize = chunk->payloadSize;
    input_read_pos_ = 0;  // Reset read position for new decode

    // Reset output buffer size to 0 (but keep capacity for reuse)
    pcm_chunk_->payload = static_cast<char*>(realloc(pcm_chunk_->payload, 0)); // NOLINT
    pcm_chunk_->payloadSize = 0;
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
    return true;
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

        flacDecoder->pcm_chunk_->payload = static_cast<char*>(realloc(flacDecoder->pcm_chunk_->payload, flacDecoder->pcm_chunk_->payloadSize + bytes));

        for (size_t channel = 0; channel < flacDecoder->sample_format_.channels(); ++channel)
        {
            if (buffer[channel] == nullptr)
            {
                LOG(ERROR, LOG_TAG) << "ERROR: buffer[" << channel << "] is NULL\n";
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }

            if (flacDecoder->sample_format_.sampleSize() == 1)
            {
                auto* chunkBuffer = reinterpret_cast<int8_t*>(flacDecoder->pcm_chunk_->payload + flacDecoder->pcm_chunk_->payloadSize);
                for (size_t i = 0; i < frame->header.blocksize; i++)
                    chunkBuffer[flacDecoder->sample_format_.channels() * i + channel] = static_cast<int8_t>(buffer[channel][i]);
            }
            else if (flacDecoder->sample_format_.sampleSize() == 2)
            {
                auto* chunkBuffer = reinterpret_cast<int16_t*>(flacDecoder->pcm_chunk_->payload + flacDecoder->pcm_chunk_->payloadSize);
                for (size_t i = 0; i < frame->header.blocksize; i++)
                    chunkBuffer[flacDecoder->sample_format_.channels() * i + channel] = SWAP_16((int16_t)(buffer[channel][i]));
            }
            else if (flacDecoder->sample_format_.sampleSize() == 4)
            {
                auto* chunkBuffer = reinterpret_cast<int32_t*>(flacDecoder->pcm_chunk_->payload + flacDecoder->pcm_chunk_->payloadSize);
                for (size_t i = 0; i < frame->header.blocksize; i++)
                    chunkBuffer[flacDecoder->sample_format_.channels() * i + channel] = SWAP_32((int32_t)(buffer[channel][i]));
            }
        }
        flacDecoder->pcm_chunk_->payloadSize += static_cast<uint32_t>(bytes);
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
