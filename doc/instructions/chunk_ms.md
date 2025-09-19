# chunk_ms

## setting

Default in snapserver.conf:

```
chunk_ms = 20
```

This setting controls the size of audio chunks sent from server to client, in milliseconds. The default is 20ms, which is a good compromise between latency and overhead. Lower values reduce latency but increase CPU usage and network overhead, while higher values do the opposite.

On stream setting:

```
source = pipewire://?name=pw-sink&chunk_ms=200
```

## In code

Search for 'chunk_ms' or 'streamChunkMs' (config setting).

### clipping experiment in server/streamreader/pcm_stream.cpp

The following code change was made to experiment with clipping message sizes to exact 4KB multiples for zero-copy networking tests. This is not a standard feature and was added for testing purposes. 

Other than expected, this change does not cramble audio. Even with this, the complete audio stream is transmitted.

```diff
index d6794b1f..7f8b0a0f 100644
@@ -62,7 +62,34 @@ PcmStream::PcmStream(PcmStream::Listener* pcmListener, boost::asio::io_context&
     if (uri_.query.find(kUriSampleFormat) == uri_.query.end())
         throw SnapException("Stream URI must have a sampleformat");
     sampleFormat_ = SampleFormat(uri_.query[kUriSampleFormat]);
-    chunk_ = std::make_unique<msg::PcmChunk>(sampleFormat_, chunk_ms_);
+    // Check for zero-copy chunk size multiplier (to create >4KB messages)
+    uint32_t zerocopy_multiplier = 1;
+    if (uri_.query.find("zerocopy_multiplier") != uri_.query.end())
+        zerocopy_multiplier = std::max(1u, static_cast<uint32_t>(cpt::stoul(uri_.query["zerocopy_multiplier"])));
+    
+    // Apply multiplier to create larger chunks for zero-copy networking
+    uint32_t effective_chunk_ms = chunk_ms_ * zerocopy_multiplier;
+    chunk_ = std::make_unique<msg::PcmChunk>(sampleFormat_, effective_chunk_ms);
+    
+    // For testing: clip TOTAL message size (payload + headers) to exact 4KB multiples
+    constexpr size_t FOUR_KB = 4096;
+    constexpr size_t HEADERS_SIZE = 12; // timestamp (8) + payloadSize (4)
+    size_t original_payload = chunk_->payloadSize;
+    size_t original_total = original_payload + HEADERS_SIZE;
+    
+    // Find largest 4KB multiple that fits: total_message = n * 4096
+    size_t target_total = (original_total / FOUR_KB) * FOUR_KB;
+    if (target_total < HEADERS_SIZE) target_total = FOUR_KB; // Ensure at least 1x4KB
+    
+    size_t target_payload = target_total - HEADERS_SIZE;
+    
+    if (target_payload != original_payload) {
+        chunk_->payloadSize = target_payload;
+        chunk_->payload = static_cast<char*>(realloc(chunk_->payload, target_payload));
+        LOG(INFO, LOG_TAG) << "TESTING: Clipped total message from " << original_total << " to " << target_total << " bytes (" << (target_total/FOUR_KB) << " x 4KB), payload: " << original_payload << " -> " << target_payload << "\n";
+    }
+    
+    LOG(INFO, LOG_TAG) << "Zero-copy multiplier: " << zerocopy_multiplier << ", effective chunk duration: " << effective_chunk_ms << "ms, final size: " << chunk_->payloadSize << " bytes\n";
     silent_chunk_ = std::vector<char>(chunk_->payloadSize, 0);
     LOG(DEBUG, LOG_TAG) << "Chunk duration: " << chunk_->durationMs() << " ms, frames: " << chunk_->getFrameCount() << ", size: " << chunk_->payloadSize
                         << "\n";
```

However, from this I don't like:

* zerocopy_multiplier as config parameter
  (I think this is not the right way, it should use chunk_ms directly)
* Copy the payload to silent_chunk_ is against zero-copy idea
  (there must be a better way)
* realloc(): same problem
  (it would be better to create the chunk with the right size from the beginning)

Observations:

* Setting chunk_ms only will not work to increase the wire chunk size.
* This perhaps because in `server/streamreader/pcm_stream.cpp` around line 78:
  ```cpp
      if (uri_.query.find(kUriChunkMs) != uri_.query.end())
        chunk_ms_ = cpt::stoul(uri_.query[kUriChunkMs]);
  ```
  The chunk_ms_ is set from the URI query parameter, BUT is is already used on line 65:
    ```cpp
        chunk_ = std::make_unique<msg::PcmChunk>(sampleFormat_, chunk_ms_);
    ```
* The calculation of wire net chunk size is not correct and working.

## Directions

* We need to control the wire chunk size with chunk_ms, as it was originally intended.
  (but see below)
* We need to avoid copying the payload for silent chunks.
* My picture of the flow is like that:
  + chunk_ms is used for PCM
  + but (w.r.t. the setting) PCM is then encoded (normally with FLAC)
  + hence the wire chunk size is (normally) smaller then chunk_ms
  + however, on the client side, the wire chunk is decoded back to PCM,
    hence the client PCM chunk size is again chunk_ms

### Implementation plan

* Phase 1: Ensure that chunk_ms is used for PCM (wire chunk size is smaller)
* Phase 2: From the experiment, we know that sending exact 4KB multiples works well
  + We should use this idea but implement it with ZC or other tweaks.
  + For this, we need to understand better why the experiement has worked.

