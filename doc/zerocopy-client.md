# Snapcast Client Zero-Copy Implementation

This document describes the zero-copy receive implementation for Snapcast client networking, providing enhanced performance through reduced memory copying for large audio chunk reception.

## Overview

The client zero-copy implementation focuses on **receiving** audio chunks efficiently from the server, complementing the server's zero-copy **sending** implementation documented in [doc/zerocopy-server.md](zerocopy-server.md). 

**Key Architecture Difference**:
- **Server**: Uses MSG_ZEROCOPY for **sending** audio chunks to clients
- **Client**: Uses direct `recv()` for **receiving** audio chunks, avoiding boost::asio buffer copies

Both implementations share the same atomic coordination patterns, statistics tracking, and logging formats for consistency.

## Configuration

### Command Line Option

Enable zero-copy networking with the `-z` flag:

```bash
./bin/snapclient -z tcp://server_ip:1704
```

The `-z` flag enables zero-copy receive for large audio chunks (≥1024 bytes) while maintaining regular async operations for small control messages.

### Usage Examples

```bash
# Enable zero-copy receive via command line
./bin/snapclient -z tcp://192.168.1.100:1704

# Works with mDNS discovery too
./bin/snapclient -z
```

## Implementation Details

### Client-Specific Architecture

The client implementation follows the same **coordinated approach** as documented in [zerocopy-server.md](zerocopy-server.md), but adapted for receive operations:

**ClientConnectionTcpZeroCopy** (`client/client_connection_tcp_zerocopy.hpp/cpp`)
- Enhanced TCP connection that coordinates zero-copy receive with Boost.Asio async operations
- Uses direct `recv()` calls for large messages (≥1024 bytes) to avoid buffer copying
- Falls back to regular `boost::asio::async_read()` for small messages or when coordination prevents zero-copy
- Provides identical diagnostics and statistics format as the server

### Race Condition Avoidance

The client uses the **identical atomic coordination mechanism** described in [zerocopy-server.md](zerocopy-server.md):

**Atomic Compare-and-Swap Pattern**
```cpp
bool ClientConnectionTcpZeroCopy::tryReserveZeroCopy()
{
    uint32_t expected = 0;
    while (!pending_async_operations_.compare_exchange_weak(expected, 1))
    {
        if (expected != 0)
            return false; // Another operation is in progress
        // Retry on spurious failures
    }
    return true; // Successfully reserved zerocopy
}
```

**Coordination Points**:
- **Regular Async Operations**: Increment counter before async_read, decrement in completion handler
- **Zero-Copy Operations**: Reserve atomically, release when complete
- **Mutual Exclusion**: Same atomic counter ensures exclusive access

### Technical Implementation Differences

#### Zero-Copy Receive vs Send
Unlike the server's MSG_ZEROCOPY sending approach, the client uses:

```cpp
// Direct recv() to avoid boost::asio intermediate buffer copying
ssize_t result = recv(native_socket_, zerocopy_buffer_.get(), message_size, MSG_DONTWAIT);
```

**Benefits**:
- **Reduced Memory Copies**: Direct socket → application buffer (bypasses boost::asio buffers)
- **Large Message Efficiency**: Particularly effective for audio chunks (typically >1KB)
- **Coordination Safety**: Same atomic patterns prevent async operation conflicts

#### Operation Flow (Client Receive)
1. **Message Header**: Always use regular async_read (headers are small ~32 bytes)
2. **Large Message Bodies (≥1024 bytes)**: Try coordinated zero-copy receive, fallback to regular if busy
3. **Small Message Bodies (<1024 bytes)**: Use coordinated regular async_read
4. **Busy Socket**: Queue operations using atomic coordination counter

## Diagnostics System

### Statistics Tracking

The client provides **identical statistics format** as documented in [zerocopy-server.md](zerocopy-server.md), adapted for receive operations:

```cpp
struct ZeroCopyStats {
    uint64_t zerocopy_attempts{0};           // Total zero-copy receive attempts
    uint64_t zerocopy_successful{0};         // Successful zero-copy receives
    uint64_t zerocopy_bytes{0};              // Total bytes received via zero-copy
    uint64_t regular_receives{0};            // Messages received via async_read
    uint64_t regular_bytes{0};               // Total bytes received via async_read
    uint64_t coordination_fallbacks{0};     // Fallbacks due to pending async ops
    uint64_t pending_async_operations{0};   // Currently pending async operations
    uint64_t outstanding_zerocopy_buffers{0}; // Outstanding zerocopy operations
    // ... additional server-compliant metrics
};
```

### Periodic Reporting

**Identical 30-second reporting format** as server:

```
=== Client ZeroCopy Receive Status (every 30s) ===
ZC Attempts: 1250, ZC Successful: 1200, ZC Bytes: 15728640, 
Regular Receives: 45, Regular Bytes: 2048, Coordination Fallbacks: 5, 
Pending Async Operations: 0, Outstanding ZC Buffers: 0,
ZC Success Rate: 96.00%, Completion Reliability: 100.00%
```

**Key Differences from Server Logs**:
- "Regular Receives" instead of "Regular Sends"
- "Receive" terminology throughout
- Same statistical meaning and format

## Testing and Verification

### How to Test Zero-Copy Receive

1. **Start server** (with or without zero-copy):
   ```bash
   ./bin/snapserver
   ```

2. **Start client with zero-copy receive enabled**:
   ```bash
   ./bin/snapclient -z tcp://server_ip:1704
   ```

3. **Monitor client logs** for zero-copy receive diagnostics

### Log Interpretation

#### Connection Messages
- `"Creating zero-copy TCP connection for RECEIVE"` - Zero-copy receive enabled
- `"Zero-copy receive enabled, starting periodic logging"` - Initialization successful

#### Performance Indicators

**High Performance (Working Well)**:
```
ZC Success Rate: 98.20%
ZC Successful: 1200
Coordination Fallbacks: 5
```

**Coordination Issues**:
```
ZC Success Rate: 45.00%
Coordination Fallbacks: 550  # High fallback rate indicates busy socket
```

#### Understanding Client-Specific Metrics

- **High Coordination Fallbacks**: Normal during active streaming due to frequent small control messages
- **Zero ZC Attempts**: All messages below 1024-byte threshold (check audio format)
- **High Regular Receives**: Expected for control messages, metadata, and small chunks

## Troubleshooting

### Zero-Copy Receive Not Working

Refer to the troubleshooting section in [zerocopy-server.md](zerocopy-server.md) for general kernel and system requirements.

**Client-Specific Issues**:

1. **No large messages**: Check audio stream format - compressed audio may result in chunks <1024 bytes
2. **Connection type**: Zero-copy only applies to TCP connections (not WebSocket/WSS)
3. **Buffer allocation**: Monitor memory usage for large audio chunk handling

### Performance Optimization

1. **Audio Format**: Uncompressed PCM formats benefit most from zero-copy receive
2. **Chunk Size**: Server chunk size settings affect zero-copy utilization
3. **Network Conditions**: High latency networks may see different coordination patterns

## Integration Notes

- **Fully Server-Compatible**: Uses identical coordination and statistics patterns
- **Backward Compatible**: Automatic fallback to regular async operations
- **Thread Safe**: Same atomic operations as server implementation
- **Boost.Asio Compatible**: Seamless integration with existing async patterns
- **Memory Optimized**: Shares buffer pool with server architecture

## Performance Benefits

When working correctly with large audio chunks:

- **Reduced Memory Copying**: Eliminates boost::asio intermediate buffer copying
- **Lower CPU Usage**: Direct socket → application buffer transfers
- **Improved Cache Efficiency**: Fewer memory operations for large audio data
- **Maintained Async Benefits**: Preserves all Boost.Asio advantages
- **Safe Coordination**: Same race-condition prevention as server

## Historical Note

An earlier implementation focused on zero-copy **sending** for the client (available in git tag `zc-client-send`). This approach was incorrect since clients primarily **receive** large audio chunks and only send small control messages. The current implementation correctly focuses on receive optimization where the performance benefits are most significant.

## Acknowledgements

This implementation maintains full compliance with the server's coordination patterns and diagnostic systems documented in [zerocopy-server.md](zerocopy-server.md). The atomic coordination mechanism, statistics tracking, and logging formats are designed for consistency across both client and server components.