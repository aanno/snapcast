# Developing with RIST in Snapcast

This document covers key insights and gotchas when implementing RIST (Reliable Internet Stream Transport) bidirectional communication in Snapcast.

## Overview

RIST provides reliable, low-latency streaming over IP networks with built-in error recovery. Snapcast uses libRIST for bidirectional communication between server and clients, supporting both audio/control data (server→client) and backchannel data (client→server).

## Architecture Pattern

### Dual-Context Approach ✅

**Correct**: Use separate sender and receiver contexts for bidirectional communication.

```cpp
// Server
struct rist_ctx* sender_ctx_;    // For sending audio/control to clients
struct rist_ctx* receiver_ctx_;  // For receiving backchannel from clients

// Client  
struct rist_ctx* receiver_ctx_;  // For receiving audio/control from server
struct rist_ctx* sender_ctx_;    // For sending backchannel to server
```

**Incorrect**: Attempting to use a single context for bidirectional communication.
```cpp
// ❌ This doesn't work - libRIST separates sender/receiver roles
struct rist_ctx* rist_ctx_;  // Cannot do both send and receive
```

## Port Configuration

### Separate Ports Required ✅

Each context needs its own port to avoid binding conflicts:

```cpp
// Server configuration
sender_url = "rist://@0.0.0.0:1706";    // Bind sender to port 1706
receiver_url = "rist://@0.0.0.0:1707";  // Bind receiver to port 1707

// Client configuration  
receiver_url = "rist://server:1706";    // Connect to server's sender
sender_url = "rist://server:1707";      // Connect to server's receiver
```

**Common Error**: Both contexts trying to bind to the same port:
```cpp
// ❌ This causes "Address already in use" errors
sender_url = "rist://@0.0.0.0:1706";
receiver_url = "rist://@0.0.0.0:1706";  // Same port!
```

## Virtual Port Multiplexing

Use virtual ports to distinguish different data types on the same connection:

```cpp
// Virtual port assignments
static constexpr uint16_t VPORT_AUDIO = 1000;        // Audio data
static constexpr uint16_t VPORT_CONTROL = 2000;      // Control messages  
static constexpr uint16_t VPORT_BACKCHANNEL = 3000;  // Backchannel data

// Set in rist_data_block
data_block.virt_src_port = VPORT_CONTROL;
data_block.virt_dst_port = VPORT_CONTROL;
```

## Connection Management

### Status Callbacks

Always set connection status callbacks to monitor connection health:

```cpp
rist_connection_status_callback_set(sender_ctx_, senderCallback, this);
rist_connection_status_callback_set(receiver_ctx_, receiverCallback, this);
```

Handle these status events:
- `RIST_CONNECTION_ESTABLISHED` / `RIST_CLIENT_CONNECTED`
- `RIST_CONNECTION_TIMED_OUT` / `RIST_CLIENT_TIMED_OUT`

### Context Type Matching

**Critical**: Use correct functions for each context type:

```cpp
// ✅ Sender context
rist_sender_create(&sender_ctx_, ...);
rist_sender_data_write(sender_ctx_, &data_block);

// ✅ Receiver context  
rist_receiver_create(&receiver_ctx_, ...);
rist_receiver_data_read2(receiver_ctx_, &data_block, timeout);

// ❌ Wrong context type calls
rist_sender_data_write(receiver_ctx_, ...);  // ERROR!
rist_receiver_data_read2(sender_ctx_, ...);  // ERROR!
```

## Message Processing

### Server Backchannel Processing

When receiving backchannel messages, properly parse Snapcast protocol:

```cpp
// Process received RIST data as Snapcast messages
if (data_block->payload_len >= base_msg_size_) {
    base_message_.deserialize(reinterpret_cast<const char*>(buffer_.data()));
    
    auto message = msg::factory::createMessage(base_message_, 
                     reinterpret_cast<const char*>(buffer_.data()));
    
    if (messageReceiver_ && message) {
        messageReceiver_->onMessageReceived(this, message, boost::system::error_code());
    }
}
```

## Common Pitfalls

### 1. Context Type Mismatches
**Problem**: Calling receiver functions on sender contexts or vice versa.
**Solution**: Always match function calls to context type.

### 2. Port Conflicts  
**Problem**: Multiple contexts binding to the same port.
**Solution**: Use distinct ports for each context (e.g., 1706, 1707).

### 3. Missing Connection Status Handling
**Problem**: Connections fail silently without proper status monitoring.
**Solution**: Always implement connection status callbacks.

### 4. Incomplete Message Processing
**Problem**: Receiving RIST data but not parsing as application protocol messages.
**Solution**: Implement proper message deserialization and forwarding.

### 5. Single Context Assumption
**Problem**: Expecting one RIST context to handle bidirectional communication.  
**Solution**: Always use separate sender and receiver contexts.

## Debugging Tips

### Logging Connection Events
```cpp
LOG(INFO, LOG_TAG) << "RIST sender connection established\n";
LOG(INFO, LOG_TAG) << "RIST receiver client connected\n";
LOG(DEBUG, LOG_TAG) << "Received data: " << data_block->payload_len 
                   << " bytes on virtual port " << data_block->virt_dst_port << "\n";
```

### Monitoring Connection Health
- Check for repeated connection/disconnection cycles
- Monitor for timeout errors indicating unstable connections
- Verify both sender and receiver contexts establish connections

### Testing Connectivity
1. Verify server binds to both ports successfully
2. Check client connects to both server ports  
3. Confirm bidirectional data flow with virtual port separation
4. Test connection recovery after network interruptions

## References

- [libRIST Documentation](https://code.videolan.org/rist/librist)
- Snapcast Stream Session Architecture
- Virtual Port Multiplexing Best Practices