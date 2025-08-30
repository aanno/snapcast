Here's a sketch of how to implement zero-copy decoding with libfoxenflac in C++. The design leverages libfoxenflac’s ability to decode from arbitrary sized input buffers and write output samples directly into caller-managed buffers, avoiding intermediate copies.
Key Concept:

    Provide input FLAC data chunks in-place to the decoder.

    Provide output buffer (e.g., int32_t[]) to receive decoded PCM samples directly.

    Process input incrementally and consume only processed bytes, enabling streaming zero-copy.

Sketch of Zero-Copy Style Decoder Loop

cpp
#include <cstdint>
#include <memory>
#include <vector>
#include <foxen-flac.h>  // libfoxenflac header

class FoxenFlacDecoder {
public:
    FoxenFlacDecoder() {
        flac_ = FX_FLAC_ALLOC_DEFAULT();
        if (!flac_) {
            throw std::runtime_error("Failed to allocate libfoxenflac decoder");
        }
    }

    ~FoxenFlacDecoder() {
        free(flac_);
    }

    // Initialize output buffer size (samples)
    void setOutputBufferSize(size_t samples) {
        output_buffer_.resize(samples);
    }

    // Feed input compressed FLAC data slice, decode into output buffer,
    // returns number of output samples decoded, input bytes consumed
    bool decodeChunk(const uint8_t* input_data, size_t& input_len,
                     int32_t*& out_pcm_data, size_t& out_pcm_len) {
        uint32_t out_len = output_buffer_.size();
        if (fx_flac_process(flac_, const_cast<uint8_t*>(input_data),
                           &input_len, output_buffer_.data(), &out_len) == FX_FLAC_ERR) {
            return false; // Error
        }

        out_pcm_len = out_len;
        out_pcm_data = output_buffer_.data();
        return true;
    }

private:
    fx_flac_t* flac_;
    std::vector<int32_t> output_buffer_;  // application-owned output buffer
};

Explanation

    input_data points to the compressed FLAC bytes buffer; input_len is both input size and on return, how many bytes were consumed.

    output_buffer_ is pre-allocated and reused, holding decoded PCM samples (interleaved channels).

    fx_flac_process() decodes as much as possible from input_data into output_buffer_ in-place, no intermediate copies internal to the client.

    The client manages lifetime of buffers, enabling zero-copy semantics around decoder boundaries.

Usage Example

cpp
FoxenFlacDecoder decoder;
decoder.setOutputBufferSize(1024);  // for example

uint8_t* flac_stream_data = /* pointer to FLAC compressed data */;
size_t flac_stream_len = /* available compressed bytes */;

while (flac_stream_len > 0) {
    int32_t* pcm_output = nullptr;
    size_t pcm_samples = 0;

    if (!decoder.decodeChunk(flac_stream_data, flac_stream_len, pcm_output, pcm_samples)) {
        // handle error
        break;
    }

    if (pcm_samples > 0) {
        // pcm_output points to zero-copy decoded PCM samples ready for processing
        processAudioSamples(pcm_output, pcm_samples);
    }

    flac_stream_data += flac_stream_len;  // advance compressed data pointer by consumed bytes
    // flac_stream_len updated by decodeChunk to remaining bytes
}

Summary
Benefit	Explanation
Zero-copy decoding	Application owns input and output buffers, passed directly to libfoxenflac and reused
Streaming friendly	Incremental decode supports arbitrary input chunk sizes
Minimal or no internal copying	Decoder works in-place on provided buffers
Suitable for embedded and high-performance streaming use cases	Compact, low-overhead design

This pattern leverages libfoxenflac’s design for minimal buffering and zero-copy style integration, helping build efficient FLAC decoding pipelines in C++.

Reference: libfoxenflac documentation and examples
