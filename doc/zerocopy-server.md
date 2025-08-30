# Snapcast Server Zerocopy Implementation

This document describes the MSG_ZEROCOPY implementation for Snapcast server networking, providing enhanced performance through kernel-level zero-copy transmission.

## Overview

The zerocopy implementation reduces CPU usage and memory bandwidth by allowing the kernel to send data directly from application buffers without additional copying. This is particularly beneficial for streaming large PCM audio chunks to multiple clients.

## Implementation Details

### Core Components

1. **ZeroCopySocket** (`server/zerocopy_socket.hpp/cpp`)
   - Wraps Boost.Asio TCP sockets with MSG_ZEROCOPY support
   - Handles SO_ZEROCOPY socket option configuration
   - Monitors kernel error queue for completion notifications
   - Provides graceful fallback to regular TCP when zerocopy unavailable

2. **StreamSessionTcpZeroCopy** (`server/stream_session_tcp_zerocopy.hpp/cpp`)
   - Enhanced TCP session that uses zerocopy for large PCM chunks (>1024 bytes)
   - Automatically falls back to regular sends for small messages
   - Provides session-level diagnostics and statistics

3. **Stream Server Integration** (`server/stream_server.cpp`)
   - All new client connections use zerocopy sessions by default
   - Aggregates statistics across all sessions
   - Provides periodic diagnostics reporting

### Technical Implementation

- **sendmsg() with MSG_ZEROCOPY**: Uses Linux kernel zerocopy transmission
- **Error Queue Monitoring**: Async monitoring via `recvmsg(MSG_ERRQUEUE)` for completion notifications
- **Buffer Lifecycle Management**: Tracks buffer usage until kernel confirms completion
- **Boost.Asio Integration**: Full compatibility with existing async IO patterns

## Comprehensive Diagnostics System

### Real-time Statistics Tracking

The implementation includes comprehensive statistics with atomic counters for thread safety:

- **Zerocopy Operations**: Count and bytes sent via MSG_ZEROCOPY
- **Regular Operations**: Count and bytes sent via standard TCP
- **Completion Notifications**: Successful kernel completion confirmations
- **Fallback Events**: When zerocopy falls back to regular TCP (EAGAIN/EWOULDBLOCK)
- **Pending Buffers**: Currently queued zerocopy buffers awaiting completion

### Logging Integration

The diagnostics integrate with Snapcast's existing logging infrastructure:

#### Log Levels
- **INFO**: Periodic summaries and diagnostics reports
- **DEBUG**: Individual zerocopy operations and fallback events
- **NOTICE**: Connection establishment and zerocopy enablement status

#### Configuration
Uses existing `snapserver.conf` logging configuration:
```ini
[logging]
sink = stdout
filter = *:trace  # Shows all log levels including zerocopy diagnostics
```

### Periodic Reporting

Every 30 seconds, when clients are connected, the server automatically logs:

#### Server-Level Statistics
```
=== ZeroCopy Diagnostics Summary ===
Active sessions: 2
ZeroCopy enabled sessions: 2
Total zerocopy sends: 1250 (15728640 bytes)
Total regular sends: 45 (2048 bytes)  
Overall zerocopy ratio: 96.5% (by count), 99.9% (by bytes)
```

#### Individual Session Details
```
ZeroCopy Socket Diagnostics for 192.168.1.100:
  Runtime: 120s
  Zerocopy enabled: YES
  Zerocopy sends: 625 (7864320 bytes)
  Regular sends: 23 (1024 bytes)
  Completions: 620
  Fallbacks: 2
  Pending buffers: 3
  Zerocopy ratio: 96.5% (by count), 99.9% (by bytes)
```

## Testing and Verification

### How to Test Zerocopy

1. **Start the server**: 
   ```bash
   scripts/run-snapserver.sh
   ```

2. **Connect clients**:
   ```bash
   scripts/run-snapclient.sh
   ```

3. **Monitor logs** for zerocopy diagnostics messages

### What the Logs Tell You

#### Connection Messages
- `"MSG_ZEROCOPY enabled successfully"` - Zerocopy is working
- `"Failed to enable SO_ZEROCOPY: <reason>"` - Kernel doesn't support zerocopy

#### Performance Indicators

**High Performance (Working Well)**:
- High zerocopy ratio (>90%)
- Low fallback count
- Completion count close to zerocopy sends count
- Large byte ratios for zerocopy

**Potential Issues**:
- High fallback count - Network/kernel conditions causing problems
- Zero zerocopy sends - MSG_ZEROCOPY not available or failing
- Low completion rate - Possible kernel notification issues

#### Example Good Performance
```
Zerocopy ratio: 98.2% (by count), 99.8% (by bytes)
Completions: 1200
Fallbacks: 5
```

#### Example Poor Performance
```
Zerocopy ratio: 15.0% (by count), 45.2% (by bytes)  
Completions: 150
Fallbacks: 850
```

## Troubleshooting

### Zerocopy Not Available
If zerocopy is not working:

1. **Check kernel version**: MSG_ZEROCOPY requires Linux kernel 4.14+
2. **Check socket buffer limits**: May need to increase `net.core.optmem_max`
3. **Check permissions**: Some containers may restrict socket options
4. **Review logs**: Look for SO_ZEROCOPY enablement failures

### Performance Issues

1. **High fallback rate**: 
   - Network congestion causing EAGAIN/EWOULDBLOCK
   - Insufficient socket buffer space
   - High client load

2. **Low completion rate**:
   - Kernel notification issues
   - Error queue monitoring problems
   - Buffer lifecycle management issues

### Log Configuration

To adjust diagnostic verbosity, modify `snapserver.conf`:

```ini
[logging]
# Show only INFO and above (less verbose)
filter = *:info

# Show DEBUG and above (more verbose)  
filter = *:debug

# Show all levels (most verbose)
filter = *:trace
```

## Performance Benefits

When working correctly, MSG_ZEROCOPY provides:

- **Reduced CPU usage**: Eliminates memory copying in kernel
- **Lower memory bandwidth**: Direct buffer transmission
- **Better scalability**: More efficient with multiple clients
- **Reduced latency**: Fewer copy operations in data path

The diagnostics system allows real-time monitoring of these benefits and identification of any performance degradation.

## Integration Notes

- **Backward Compatible**: Automatically falls back to regular TCP when zerocopy unavailable
- **Thread Safe**: All statistics use atomic operations
- **Boost.Asio Compatible**: Full integration with existing async patterns
- **No Configuration Required**: Works out-of-the-box with automatic detection