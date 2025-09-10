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

## LibRIST API Version Compatibility ⚠️

**Important**: libRIST has different API versions. The examples below show **newer API** patterns that may not be available in older libRIST installations.

### Newer API Pattern (libRIST v0.3+)
```cpp
// This pattern works with newer libRIST versions
rist_ctx *sender_ctx, *receiver_ctx;
rist_peer_config sender_cfg = {}, receiver_cfg = {};
sender_cfg.url = "rist://@0.0.0.0:1706";
rist_sender_create(&sender_ctx, &sender_cfg);
rist_receiver_set_callback(receiver_ctx, callback);
```

### Older API Pattern (libRIST v0.2.x) ✅ **Currently Used**
```cpp
// This is what Snapcast currently uses (works with older libRIST)
rist_sender_create(&sender_ctx, RIST_PROFILE_MAIN, 0, nullptr);
rist_receiver_create(&receiver_ctx, RIST_PROFILE_MAIN, nullptr);

// URL configuration via parsing
struct rist_peer_config* config = nullptr;
rist_parse_address2("rist://@0.0.0.0:1706", &config);
rist_peer_create(sender_ctx, &peer, config);

// Callbacks via callback_set2
rist_receiver_data_callback_set2(receiver_ctx, callback, this);
```

**Recommendation**: Use the older API pattern for maximum compatibility until libRIST versions are standardized across distributions.

## Timeout Configuration Issues ⚠️

**Critical Discovery**: libRIST v0.2.7 has a data output thread delay (~1 second) that affects callback/polling data delivery. This causes timeout issues with Snapcast's default 2-second request timeouts.

### Problem
```cpp
// Default timeouts are too short for libRIST data delivery
clientConnection_->sendRequest(hello, 2s, handler);          // ❌ Too short
clientConnection_->sendRequest<msg::Time>(timeReq, 2s, ...); // ❌ Too short
```

### Solution
```cpp
// Increased timeouts to accommodate libRIST delay
clientConnection_->sendRequest(hello, 10s, handler);          // ✅ Sufficient
clientConnection_->sendRequest<msg::Time>(timeReq, 10s, ...); // ✅ Sufficient
```

**Files Modified**:
- `client/controller.cpp:491` - Hello request timeout
- `client/controller.cpp:337` - Time sync request timeout

### Callback vs Polling Fallback

Due to libRIST v0.2.7 callback reliability issues, a polling fallback is implemented:

```cpp
// Polling fallback for libRIST v0.2.7
void pollingThread() {
    while (running_) {
        struct rist_data_block* data_block = nullptr;
        int ret = rist_receiver_data_read2(receiver_ctx_, &data_block, 5);
        if (ret > 0 && data_block) {
            // Process data_block
            rist_receiver_data_block_free2(&data_block);
        }
    }
}
```

**Note**: Only log polling results when `ret != 0` or `data_block != nullptr` to reduce log noise.

## Port Conflict Resolution ✅

**Issue**: RTCP port conflicts when using adjacent ports (e.g., 1706/1707).

**Solution**: Use port + 2 separation to avoid RTCP overlap:

```cpp
// Server configuration
std::string sender_url = "rist://@" + bind_to_address + ":" + std::to_string(port);           // 1706
std::string receiver_url = "rist://@" + bind_to_address + ":" + std::to_string(port + 2);     // 1708

// Client configuration  
std::string receiver_url = "rist://" + server + ":" + std::to_string(port);                   // 1706
std::string sender_url = "rist://" + server + ":" + std::to_string(port + 2);                 // 1708
```

**Files Modified**:
- `server/stream_session_rist_bidirectional.cpp` - Changed client_port_ + 1 to client_port_ + 2
- `client/client_connection_rist_bidirectional.cpp` - Changed server_.port + 1 to server_.port + 2

## Message Processing Chain Issues 🔧

### Handler Lifecycle Problem ✅ FIXED

**Issue**: `pending_handler_` was cleared before callback execution in `processMessage()`, causing subsequent messages (like CodecHeader) to be dropped.

**Solution**: Store handler before clearing `pending_handler_`:

```cpp
if (pending_handler_) {
    auto h = pending_handler_;      // Store handler first
    pending_handler_ = nullptr;     // Clear before callback
    messageReceived(std::move(message), [this, h](boost::system::error_code ec, std::unique_ptr<msg::BaseMessage> msg) {
        h(ec, std::move(msg));
        if (!ec) getNextMessage(h);    // Chain with same handler
    });
}
```

**Files Modified**: `client/client_connection_rist_bidirectional.cpp` - processMessage()

### BaseMessage Copy Crash ✅ FIXED

**Issue**: `BaseMessage` is not safely copyable, causing segmentation faults when trying to copy for both pending request resolution and handler callback.

**Solution**: Avoid copying `BaseMessage` entirely. For responses to pending requests, skip handler callback temporarily:

```cpp
void messageReceived(std::unique_ptr<msg::BaseMessage> message, MessageHandler<msg::BaseMessage> handler) override {
    // Check for pending request
    if (req->id() == message->refersTo) {
        req->setValue(std::move(message));  // Move to pending request
        getNextMessage(handler);            // Chain without calling handler
        return;
    }
    // Normal message path
    handler({}, std::move(message));
}
```

**Files Modified**: 
- `client/client_connection.hpp` - Made `messageReceived` virtual
- `client/client_connection_rist_bidirectional.hpp` - Added override
- `client/client_connection_rist_bidirectional.cpp` - Implemented safe messageReceived

### Stream Buffer Age Calculation Issue ✅ RESOLVED

**Final Issue**: Even with all RIST transport working correctly, audio chunks were being dropped as "too old" due to libRIST's ~100ms transport latency vs TCP's near-zero latency.

**Root Cause**: 
- RIST introduces 60-120ms network transport latency
- Snapcast's Stream buffer expects chunks to arrive "just in time"  
- Age calculation: `age = serverNow - chunk.start - buffer + outputBufferDacTime`
- Without compensation, chunks appear 100ms "older" than expected

**Solution**: Use snapclient `--latency` parameter to compensate for RIST transport latency:

```bash
snapclient rist://server:1706 --latency 100
```

**Why This Works**:
- `--latency 100` increases `outputBufferDacTime` by 100ms
- This makes chunks appear "100ms younger" in age calculation
- Chunks that were 36-348ms "old" become playable
- Uses existing Snapcast architecture designed for exactly this purpose

**Key Insight**: The solution wasn't changing buffer sizes or modifying age thresholds, but using the proper latency compensation mechanism already built into Snapcast.

**Files Modified**: Client startup scripts to include `--latency 100` parameter

## References

- [libRIST Documentation](https://code.videolan.org/rist/librist)
- Snapcast Stream Session Architecture
- Virtual Port Multiplexing Best Practices