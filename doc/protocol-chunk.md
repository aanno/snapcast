# Snapcast Wire Protocol Enhancement - Independent chunk_ms and chunk_kb

This document describes the enhanced wire protocol that introduces independent control over PCM audio timing (`chunk_ms`) and network wire block sizes (`chunk_kb`) for optimal zero-copy performance.

## Overview

The enhanced protocol separates audio timing concerns from network optimization by introducing two independent parameters:

- **`chunk_ms`**: Controls PCM audio chunk duration (audio timing)
- **`chunk_kb`**: Controls wire block size for network transmission (zero-copy optimization)

## Architecture Changes

### Traditional Protocol (Before)
```
PCM Stream → FLAC Encoding → Variable Wire Chunks → Client
chunk_ms=20    ~3KB FLAC      1-4KB messages     Decode
```

### Enhanced Protocol (After)
```
PCM Stream → FLAC Encoding → Wire Block Accumulation → Fixed Wire Blocks → Client
chunk_ms=20    ~3KB FLAC      Pack into blocks       4KB/8KB messages    Reconstruct → Decode
```

## Parameter Independence

The key innovation is that `chunk_ms` and `chunk_kb` are now **completely independent**:

### chunk_ms (Audio Timing)
- **Purpose**: Controls PCM audio chunk duration for timing accuracy
- **Default**: 20ms 
- **Range**: 1-1000ms
- **Impact**: Audio latency, timing precision, CPU overhead
- **Example**: `chunk_ms=10` creates 10ms PCM chunks for low-latency applications

### chunk_kb (Network Optimization) 
- **Purpose**: Controls wire block size for zero-copy networking
- **Default**: Not set (traditional variable-size chunks)
- **Range**: 1-64KB (practical range 4-16KB)
- **Impact**: Network efficiency, zero-copy performance, bandwidth utilization
- **Example**: `chunk_kb=4` creates 4KB wire blocks for optimal TCP_ZEROCOPY_RECEIVE

## Configuration Examples

### Traditional Mode (chunk_ms only)
```bash
# 20ms PCM chunks, variable wire sizes (1-4KB)
source = pipewire://?name=pw-sink&chunk_ms=20
```

### Zero-Copy Optimized (both parameters)
```bash
# 20ms PCM chunks packed into 4KB wire blocks
source = pipewire://?name=pw-sink&chunk_ms=20&chunk_kb=4

# Low-latency audio with large wire blocks
source = pipewire://?name=pw-sink&chunk_ms=10&chunk_kb=8

# High-latency, efficient networking
source = pipewire://?name=pw-sink&chunk_ms=50&chunk_kb=16
```

### Network-Only Optimization
```bash
# Default 20ms PCM timing with 4KB wire blocks
source = pipewire://?name=pw-sink&chunk_kb=4
```

## Implementation Details

### Server-Side Wire Block Accumulation

When `chunk_kb` is specified, the server:

1. **Creates PCM chunks** using `chunk_ms` (maintains audio timing)
2. **Encodes to FLAC** (compression varies with audio content)
3. **Accumulates FLAC chunks** in configurable-size wire blocks
4. **Sends fixed-size wire blocks** when full or on timeout

**Wire Block Structure**:
```cpp
struct WireBlock {
    // BaseMessage header (26 bytes)
    uint16_t type, id, refersTo;
    tv sent, received;
    uint32_t size;
    
    // WireBlock-specific header (16 bytes) 
    tv timestamp;                    // Audio timing
    uint32_t sequence_number;        // Block ordering
    uint32_t payload_length;         // Actual data size
    
    // Variable payload (chunk_kb * 1024 - 42 bytes)
    char payload[payload_size];      // FLAC chunk data
};
```

**Total overhead**: 42 bytes per wire block

### Client-Side Wire Block Reconstruction

When receiving wire blocks, the client:

1. **Receives fixed-size wire blocks** via zero-copy networking
2. **Reconstructs FLAC chunks** from fragmented wire block data
3. **Decodes FLAC to PCM** (standard audio processing)
4. **Maintains timing** using original timestamps

## Performance Benefits

### Network Efficiency
- **Zero-copy networking**: Fixed 4KB/8KB blocks work optimally with `TCP_ZEROCOPY_RECEIVE`
- **Reduced syscalls**: Larger, predictable message sizes
- **Better bandwidth utilization**: Consistent block sizes reduce network fragmentation

### Audio Quality Preservation  
- **Timing accuracy**: `chunk_ms` preserves precise audio timing
- **Latency control**: Independent from network block size
- **Quality maintenance**: No audio degradation from wire protocol changes

### Resource Optimization
- **Memory efficiency**: Fixed-size allocations for zero-copy buffers
- **CPU efficiency**: Reduced memory copying in network stack
- **Predictable performance**: Consistent message sizes for resource planning

## Logging and Diagnostics

### Server-Side Logging
```
[Info] (PcmStream) PcmStream: pw-sink, chunk_ms: 20ms (PCM timing), chunk_kb: 4KB (wire blocks: 4096 bytes)
[Info] (PcmStream) PCM READ: pw-sink, size: 3840 bytes, duration: 20.00ms, frames: 960
[Info] (PcmStream) FLAC ENCODED: pw-sink, duration: 24.00ms, PCM->FLAC size: 4608->3106 bytes, compression: 32.55%
[Info] (StreamSrv) CHUNK_KB MODE: Adding FLAC chunk to 4KB wire blocks, size: 3106 bytes
[Info] (StreamSrv) WIRE BLOCK SEND: sequence 42, payload 3890/4054 bytes (95% full)
```

### Client-Side Logging (Planned)
```
[Info] (Controller) WIRE BLOCK RECEIVED: sequence 42, size: 4096 bytes, payload: 3890 bytes
[Info] (Reconstructor) FLAC CHUNK RECONSTRUCTED: size: 3106 bytes from wire blocks
[Info] (FlacDec) FLAC DECODE COMPLETE: input 3106 -> output 4608 bytes, expansion: 148%
```

## Wire Block Size Guidelines

### Recommended Sizes

| chunk_kb | Use Case | Network Efficiency | Memory Usage | Latency Impact |
|----------|----------|-------------------|--------------|----------------|
| 4KB | Standard zero-copy | Excellent | Low | Minimal |
| 8KB | High-throughput | Very good | Medium | Low |
| 16KB | Bandwidth-constrained | Good | Higher | Medium |

### Size Calculation
```cpp
// Available payload per wire block
size_t payload_size = (chunk_kb * 1024) - 42;  // 42 bytes headers

// Examples:
// chunk_kb=4:  4096 - 42 = 4054 bytes payload
// chunk_kb=8:  8192 - 42 = 8150 bytes payload  
// chunk_kb=16: 16384 - 42 = 16342 bytes payload
```

## Backward Compatibility

### Full Compatibility Maintained
- **No chunk_kb specified**: Traditional variable-size chunks (existing behavior)
- **Only chunk_ms specified**: Standard PCM timing with variable wire sizes
- **Both specified**: Enhanced mode with optimal audio timing and network efficiency

### Migration Path
1. **Phase 1**: Deploy enhanced server (supports both modes)
2. **Phase 2**: Deploy enhanced client (reconstructs wire blocks)
3. **Phase 3**: Configure `chunk_kb` for zero-copy optimization

## Implementation Status

### ✅ Server-Side Complete
- [x] Independent parameter processing
- [x] Configurable wire block accumulation
- [x] Fixed-size wire block transmission
- [x] Comprehensive logging
- [x] Build successful

### 🚧 Client-Side In Progress
- [ ] Wire block reception and buffering
- [ ] FLAC chunk reconstruction
- [ ] Integration with existing decoder pipeline
- [ ] Client-side logging

### 📋 Testing Required
- [ ] End-to-end functionality testing
- [ ] Performance benchmarking vs traditional mode
- [ ] Zero-copy networking verification
- [ ] Audio quality validation

## Configuration Reference

### URI Parameters
```bash
# Audio timing control
chunk_ms=<milliseconds>     # PCM chunk duration (default: 20)

# Network optimization  
chunk_kb=<kilobytes>        # Wire block size (default: not set)

# Combined example
source = pipewire://?name=pw-sink&chunk_ms=20&chunk_kb=4
```

### Snapserver.conf Integration
```ini
[stream]
# Default values for all streams
chunk_ms = 20

# Zero-copy optimization (applied when chunk_kb specified in source URI)
# chunk_kb has no global default - must be specified per source
```

## Future Enhancements

### Adaptive Block Sizing
- Dynamic `chunk_kb` adjustment based on network conditions
- Automatic optimization for detected zero-copy capability

### Advanced Fragmentation
- Intelligent FLAC chunk boundary detection
- Optimized reconstruction algorithms

### Quality of Service
- Priority-based wire block transmission
- Adaptive quality based on network performance

## Conclusion

The independent `chunk_ms` and `chunk_kb` architecture provides the best of both worlds:

- **Audio Excellence**: Precise timing control with `chunk_ms`
- **Network Efficiency**: Zero-copy optimization with `chunk_kb`
- **Flexibility**: Independent tuning for different use cases
- **Compatibility**: Seamless integration with existing setups

This enhancement enables Snapcast to achieve optimal performance across diverse network conditions while maintaining pristine audio quality and timing accuracy.