# Snapcast Server Zerocopy Implementation

This document describes the MSG_ZEROCOPY implementation for Snapcast server networking, providing enhanced performance through kernel-level zero-copy transmission.

## Overview

The zerocopy implementation reduces CPU usage and memory bandwidth by allowing the kernel to send data directly from application buffers without additional copying. This is particularly beneficial for streaming large PCM audio chunks to multiple clients.

## Configuration

### Command Line Option

Enable zerocopy networking with the `-z` flag:

```bash
./bin/snapserver -z -c snapserver.conf
```

The `-z` flag is a simple boolean switch that enables zerocopy for all client connections.

### Configuration File

Zerocopy can also be configured in the config file (e.g., `snapserver.conf`):

```ini
[stream]
# Enable zerocopy networking for improved performance
zerocopy = true
```

**Note**: The command line `-z` flag overrides the config file setting when present.

### Usage Examples

```bash
# Enable via command line
./bin/snapserver -z

# Enable via config file
echo "zerocopy = true" >> /etc/snapserver.conf
./bin/snapserver

# Command line overrides config file
./bin/snapserver -z -c snapserver.conf  # zerocopy enabled
```

## Implementation Details

### Architecture: Coordinated Approach

Our implementation uses a **coordinated approach** that safely combines Boost.Asio async operations with direct MSG_ZEROCOPY syscalls. This design choice was made to:

1. **Preserve Boost.Asio Benefits**: Keep all existing async patterns, error handling, and event loop integration
2. **Avoid Complete Rewrite**: Minimize changes to existing codebase  
3. **Ensure Safety**: Prevent race conditions between async and direct socket operations
4. **Provide Graceful Fallback**: Automatic fallback when zerocopy is unavailable or inappropriate

### Core Components

**StreamSessionTcpCoordinated** (`server/stream_session_tcp_coordinated.hpp/cpp`)
- Enhanced TCP session that coordinates zerocopy with Boost.Asio async operations
- Uses direct `sendmsg()` with MSG_ZEROCOPY when socket is idle from async operations
- Falls back to regular `boost::asio::async_write()` when async operations are pending
- Provides comprehensive diagnostics and statistics

**Stream Server Integration** (`server/stream_server.cpp`)
- Creates coordinated sessions when zerocopy is enabled
- Provides periodic diagnostics reporting every 30 seconds
- Logs session creation: `"Creating zerocopy-enabled session"` vs `"Creating regular TCP session"`

### Technical Implementation

#### Coordination Mechanism
- **Atomic Counter**: `pending_async_operations_` tracks active Boost.Asio operations
- **Safe Zerocopy**: Only uses MSG_ZEROCOPY when counter is 0 (socket idle)  
- **Mutual Exclusion**: Regular async operations increment counter, blocking zerocopy
- **Race Prevention**: Atomic compare-and-swap prevents race conditions

#### Operation Flow
1. **Large Messages (≥1024 bytes)**: Try zerocopy first, fallback to async if socket busy
2. **Small Messages (<1024 bytes)**: Use regular async operations (more efficient)
3. **Busy Socket**: Queue operations and process when socket becomes idle
4. **Error Handling**: Monitor kernel error queue for zerocopy completion notifications

## Race Condition Avoidance

### The Challenge
Mixing Boost.Asio async operations with direct syscalls creates potential race conditions:
```cpp
// DANGEROUS: Check-then-act race condition
if (no_async_operations_pending) {  // ✓ Check passes
    // Another thread could start async_write here!
    sendmsg(socket, MSG_ZEROCOPY);  // ✗ Now mixing operations!
}
```

### Our Solution: Atomic Reservation

**1. Atomic Compare-and-Swap Pattern**
```cpp
bool StreamSessionTcpCoordinated::tryReserveZeroCopy()
{
    uint32_t expected = 0;
    while (!pending_async_operations_.compare_exchange_weak(expected, 1))
    {
        if (expected != 0)
            return false; // Another operation is in progress
        // Retry on spurious failures (expected reloaded with current value)
    }
    return true; // Successfully reserved zerocopy
}
```

**2. Coordination Points**
- **Regular Async Operations**: 
  - Increment counter before starting: `pending_async_operations_++`
  - Decrement in completion handler: `pending_async_operations_--`
- **Zerocopy Operations**:
  - Reserve atomically: `tryReserveZeroCopy()` (sets to 1)  
  - Release when done: `releaseZeroCopy()` (sets to 0)

**3. Race Prevention Guarantees**
- **Atomic Operations**: All counter access uses atomic operations
- **No Check-Then-Act**: The compare-and-swap is atomic, no gap for races
- **Spurious Failure Handling**: Retry loop handles `compare_exchange_weak` spurious failures
- **Mutual Exclusion**: Both systems use same counter, ensuring exclusive access

### Why Compare-and-Swap Works

The atomic `compare_exchange_weak()` operation:
1. **Atomically** checks if `pending_async_operations_ == 0` (idle)
2. **Atomically** sets it to `1` (reserved) if idle
3. **Returns false** if another operation started between check and set
4. **Handles spurious failures** by retrying in a loop

This eliminates the race window completely - there's no gap between checking and acting.

## Alternative Approaches Considered

### 1. Pure Boost.Asio Zerocopy
- **Pros**: Clean integration, no race conditions
- **Cons**: Boost.Asio doesn't natively support MSG_ZEROCOPY, would require extensive framework modifications

### 2. Separate Zerocopy Socket Class  
- **Pros**: Complete control over zerocopy operations
- **Cons**: Duplicate async infrastructure, more complex error handling, larger codebase changes

### 3. Strand-Based Serialization
- **Pros**: Boost.Asio strand ensures serialized access
- **Cons**: All operations must go through strand, potential performance impact

### 4. Mutex-Based Locking
- **Pros**: Simple exclusive access
- **Cons**: Blocking operations, potential latency impact, doesn't align with async patterns

**Our Choice**: The coordinated approach provides the best balance of safety, performance, and integration simplicity.

## Comprehensive Diagnostics System

### Real-time Statistics Tracking

The implementation includes comprehensive statistics with atomic counters for thread safety:

- **Zerocopy Operations**: Count and bytes sent via MSG_ZEROCOPY
- **Regular Operations**: Count and bytes sent via standard async_write  
- **Coordination Fallbacks**: When zerocopy is skipped due to pending async operations
- **Success Rate**: Percentage of operations using zerocopy vs regular sends

### Session-Level Statistics

Each coordinated session tracks:
```cpp
struct ZeroCopyStats {
    uint64_t zerocopy_attempts{0};      // Total zerocopy send attempts
    uint64_t zerocopy_successful{0};    // Successful zerocopy sends  
    uint64_t zerocopy_bytes{0};         // Total bytes sent via zerocopy
    uint64_t regular_sends{0};          // Messages sent via async_write
    uint64_t regular_bytes{0};          // Total bytes sent via async_write
    uint64_t coordination_fallbacks{0}; // Fallbacks due to pending async ops
    double zerocopy_percentage() const; // Success rate calculation
};
```

### Periodic Reporting

Every 30 seconds, when clients are connected, the server logs zerocopy diagnostics:

```
=== Periodic ZeroCopy Status (every 30s) ===
Zerocopy Stats for session 192.168.1.100
	ZC Attempts: 1250, 
	ZC Successful: 1200, 
	ZC Bytes: 15728640, 
	Regular Sends: 45, 
	Regular Bytes: 2048, 
	Coordination Fallbacks: 5, 
	ZC Success Rate: 96.00%
```

## Testing and Verification

### How to Test Zerocopy

1. **Start the server with zerocopy enabled**: 
   ```bash
   ./bin/snapserver -z -c snapserver.conf
   ```

2. **Connect clients**:
   ```bash
   scripts/run-snapclient.sh
   ```

3. **Monitor logs** for zerocopy diagnostics messages

### What the Logs Tell You

#### Connection Messages
- `"Creating zerocopy-enabled session for 192.168.1.100"` - Zerocopy is working
- `"Creating regular TCP session for 192.168.1.100"` - Zerocopy disabled or not working
- `"ZeroCopy setting: enabled"` - Shows zerocopy configuration status at startup

#### Performance Indicators

**High Performance (Working Well)**:
- High zerocopy success rate (>90%)
- Low coordination fallbacks
- Large byte ratios for zerocopy vs regular

**Potential Issues**:
- High coordination fallbacks - Socket frequently busy with async operations  
- Low success rate - MSG_ZEROCOPY not available or network issues
- Zero successful attempts - Kernel doesn't support zerocopy

#### Example Good Performance
```
ZC Success Rate: 98.20%
ZC Successful: 1200
Coordination Fallbacks: 5
```

#### Example Coordination Issues  
```
ZC Success Rate: 45.00%  
ZC Successful: 450
Coordination Fallbacks: 550  # High fallback rate
```

## Troubleshooting

### Zerocopy Not Available
If zerocopy is not working:

1. **Check kernel version**: MSG_ZEROCOPY requires Linux kernel 4.14+
2. **Check socket buffer limits**: May need to increase `net.core.optmem_max`
3. **Check permissions**: Some containers may restrict socket options
4. **Review startup logs**: Look for `"ZeroCopy setting: disabled"` messages

### Performance Issues

1. **High coordination fallbacks**: 
   - High async operation frequency (normal for small messages)
   - Very active client connections
   - Consider this normal behavior - the system is working as designed

2. **Low success rate with low fallbacks**:
   - Kernel zerocopy issues (EAGAIN, insufficient buffers)
   - Network congestion  
   - Check system limits and kernel messages

### Configuration Verification

To verify your configuration:

```bash
# Check command line help
./bin/snapserver -h

# Check config file options  
./bin/snapserver -hh | grep -i zerocopy
```

## Performance Benefits

When working correctly, the coordinated zerocopy approach provides:

- **Reduced CPU usage**: Eliminates memory copying in kernel for large messages
- **Lower memory bandwidth**: Direct buffer transmission
- **Better scalability**: More efficient with multiple clients  
- **Maintained Async Benefits**: Keeps all Boost.Asio advantages
- **Safe Operation**: No race conditions or undefined behavior

The diagnostics system allows real-time monitoring of these benefits and helps identify any coordination or performance issues.

## Integration Notes

- **Backward Compatible**: Automatically falls back to regular async operations when zerocopy unavailable
- **Thread Safe**: All statistics and coordination use atomic operations
- **Boost.Asio Compatible**: Full integration with existing async patterns
- **Configurable**: Enable via command line (`-z`) or config file (`zerocopy = true`)
- **Zero Code Changes**: Existing client code unchanged, transparent enhancement