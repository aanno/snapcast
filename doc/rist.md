# RIST Implementation in Snapcast

## Overview

Snapcast implements RIST (Reliable Internet Stream Transport) support using a clean, bidirectional transport architecture that aligns with RIST's native design principles. This document describes our implementation approach, architectural decisions, and the journey that led to the current design.

## Architecture

### Design Philosophy

Our RIST implementation follows the **testrist model** - a direct virtual port multiplexing approach that embraces RIST's message-oriented nature rather than forcing it into TCP's connection-oriented model.

### Core Components

#### 1. RistTransport (`common/rist_transport.hpp`)

**Location**: `common/` - shared between server and client  
**Purpose**: Bidirectional RIST transport using virtual port multiplexing

```cpp
class RistTransport {
public:
    enum class Mode { SERVER, CLIENT };
    
    // Virtual ports following testrist model
    static constexpr uint16_t VPORT_AUDIO = 1000;       // Audio data
    static constexpr uint16_t VPORT_CONTROL = 2000;     // Control messages  
    static constexpr uint16_t VPORT_BACKCHANNEL = 3000; // Backchannel data
};
```

**Key Features**:
- Mode-based configuration (SERVER binds, CLIENT connects)
- Virtual port multiplexing for clean message separation
- Callback interface (`RistTransportReceiver`) decoupled from session management
- Shared implementation reduces code duplication

#### 2. Server Integration (`server/stream_server.cpp`)

**Integration Strategy**: Parallel transport system - RIST runs alongside TCP/WebSocket without interference.

```cpp
class StreamServer : public StreamMessageReceiver, public RistTransportReceiver {
    std::unique_ptr<RistTransport> rist_transport_;
};
```

**Message Flow**:
1. **Client Hello** (vport 3000) → Server
2. **ServerSettings** (vport 2000) → Client  
3. **CodecHeader** (vport 1000) → Client
4. **Audio Chunks** (vport 1000) → Client (continuous)

#### 3. Port Assignment

Following testrist pattern for compatibility:
- **Server**: Binds to port 1706 (sender) and 1708 (receiver)
- **Client**: Connects to server port 1706 (receives) and 1708 (sends)
- **Virtual Ports**: 1000 (audio), 2000 (control), 3000 (backchannel)

## Why This Approach?

### The Problem with Session-Based RIST

Our initial implementation attempted to force RIST into Snapcast's existing session-per-client model, which led to fundamental issues:

1. **Empty `clientId` Problem**: RIST sessions were created before client connection (server startup), unlike TCP sessions created after connection
2. **Session Lifecycle Mismatch**: RIST's connectionless nature conflicted with session-based state management  
3. **Complex Message Routing**: Trying to make RIST behave like TCP connections created unnecessary complexity

### The testrist Insight

The breakthrough came from analyzing our working `testrist` implementation (120 lines vs 800+ in the complex approach):

- **Simple and Direct**: No session management complexity
- **Follows RIST Design**: Embraces message-oriented, virtual port multiplexing
- **Proven Pattern**: Server binds, client connects, virtual ports handle routing

### Benefits of Current Approach

#### 1. **Clean Architecture**
- **Zero Impact on TCP/WebSocket**: Completely separate transport path preserves existing functionality
- **Shared Code**: Common implementation between server and client reduces duplication
- **Clear Separation**: Virtual ports provide clean boundaries between message types

#### 2. **RIST-Native Design**
- **Message-Oriented**: Works with RIST's natural message flow instead of fighting it
- **Virtual Port Multiplexing**: Uses RIST's built-in routing capabilities
- **Direct Communication**: No artificial session abstraction layer

#### 3. **Maintainability**
- **Simpler Codebase**: Fewer abstractions means easier debugging and modification
- **Protocol Alignment**: Code structure matches RIST protocol design
- **Future-Proof**: Easy to extend with additional RIST features

## Implementation Details

### Server Side

```cpp
// Initialization
rist_transport_ = std::make_unique<RistTransport>(RistTransport::Mode::SERVER, this);
rist_transport_->configureServer(address, port);
rist_transport_->start();

// Audio streaming (parallel to TCP sessions)
if (rist_transport_ && isDefaultStream) {
    rist_transport_->sendAudioChunk(chunk);
}

// Client connection handling
void StreamServer::onRistClientConnected(const std::string& clientId) {
    // Send ServerSettings on vport 2000
    // Send CodecHeader on vport 1000  
}
```

### Client Side

**Architecture**: The client uses `ClientConnectionRistBidirectional` which implements the `RistTransportReceiver` interface, following the same direct communication pattern as the server.

```cpp
class ClientConnectionRistBidirectional : public ClientConnection, public RistTransportReceiver {
    std::unique_ptr<RistTransport> rist_transport_;
};
```

**Key Implementation Details**:

1. **Connection Establishment**:
   ```cpp
   // Client connects to server's RIST ports
   rist_transport_ = std::make_unique<RistTransport>(RistTransport::Mode::CLIENT, this);
   rist_transport_->configureClient(server_address, server_port);  // connects to 1706/1708
   rist_transport_->start();
   ```

2. **Message Sending** (Fixed Implementation):
   ```cpp
   void write(boost::asio::streambuf& buffer, WriteHandler&& write_handler) {
       // Send raw serialized data directly (no re-serialization)
       const auto* data_ptr = boost::asio::buffer_cast<const char*>(buffer.data());
       size_t data_size = buffer.size();
       
       // Use sendRawData to avoid JSON parse errors
       bool success = rist_transport_->sendRawData(RistTransport::VPORT_BACKCHANNEL, data_ptr, data_size);
   }
   ```

3. **Message Receiving**:
   ```cpp
   void onRistMessageReceived(const msg::BaseMessage& baseMessage, 
                             const std::string& payload, uint16_t vport) {
       // Handle ServerSettings, CodecHeader, Time responses
       auto message = msg::factory::createMessage(baseMessage, const_cast<char*>(payload.data()));
       messageReceived(std::move(message), handler); // Use base ClientConnection correlation
   }
   ```

**Critical Fix - sendRawData() Method**:
The major breakthrough was implementing `RistTransport::sendRawData()` to send pre-serialized message data directly, avoiding the JSON parse errors that occurred when trying to deserialize and re-serialize messages in the client's `write()` method.

**Protocol Flow**:
1. **Hello Request**: Client sends Hello via vport 3000 (backchannel)
2. **ServerSettings Response**: Server responds via vport 2000 (control)  
3. **CodecHeader**: Server sends via vport 1000 (audio)
4. **Audio Streaming**: Continuous PcmChunks via vport 1000

### Message Protocol

1. **Hello Handshake**:
   ```
   Client → Server (vport 3000): Hello{clientId}
   Server → Client (vport 2000): ServerSettings  
   Server → Client (vport 1000): CodecHeader
   ```

2. **Audio Streaming**:
   ```
   Server → Client (vport 1000): PcmChunk (continuous)
   ```

3. **Control Messages**:
   ```
   Client ↔ Server (vport 3000): Time, Status, etc.
   ```

## Alternatives Considered

### 1. Session-Based Approach (Initial Implementation)

**What we tried**:
- `StreamSessionRistBidirectional` inheriting from `StreamSession`
- Session-per-client model matching TCP implementation
- Complex session lifecycle management

**Why it failed**:
- RIST sessions created before client connection → empty `clientId`
- Session pointer lifecycle issues preventing Hello response
- Buffer format mismatches between protocols
- Fighting RIST's connectionless nature

### 2. Hybrid Session Approach

**What we tried**:
- Keep session interface but simplify RIST handling
- Session created on-demand during Hello processing

**Why we moved away**:
- Still required complex session management
- Didn't align with RIST's natural message flow
- Added unnecessary abstraction layer

### 3. Direct Port Communication (Current)

**Why it works**:
- Embraces RIST's message-oriented design
- Simple, direct communication pattern
- No session management complexity
- Matches proven testrist pattern

## Configuration

### Server Configuration (`snapserver.conf`)

```ini
[rist]
enabled = true
bind_to_address = 192.168.10.139
port = 1706
```

### Client Configuration

Client connects to server's RIST ports automatically when RIST transport is detected.

## Debugging and Monitoring

### Key Log Messages

**Server Startup**:
```
[Info] (RistTransport) Starting RIST transport in SERVER mode
[Info] (RistTransport) Virtual ports: 1000 (audio), 2000 (control), 3000 (backchannel)
```

**Client Connection**:
```
[Info] (RistTransport) RIST Hello received from client: {clientId}
[Info] (StreamServer) Sent ServerSettings to RIST client: {clientId}  
[Info] (StreamServer) Sent CodecHeader ({size} bytes) to RIST client: {clientId}
```

**Audio Streaming**:
```
[Debug] (RistTransport) Sent message type ServerSettings (83 bytes) on vport 2000
[Debug] (RistTransport) Sent message type CodecHeader (1374 bytes) on vport 1000
```

### Common Issues

1. **Empty CodecHeader**: Ensure audio stream is active before client connects
2. **Connection Timeouts**: Check firewall settings for ports 1706/1708
3. **Audio Gaps**: Monitor RIST buffer settings and network conditions

## Performance Considerations

### Buffer Configuration

Following testrist optimized parameters:
```cpp
config->recovery_length_min = 200;  // 200ms buffer
config->recovery_length_max = 200;
config->recovery_rtt_min = 5;
config->recovery_rtt_max = 500;
config->recovery_reorder_buffer = 15;
```

### Network Efficiency

- **Virtual Port Multiplexing**: Reduces connection overhead
- **Direct Message Routing**: Minimal processing overhead
- **Optimized Logging**: Reduced spam from high-frequency audio chunks

## Future Enhancements

### Potential Improvements

1. **Multi-Stream Support**: Extend virtual port assignments for multiple audio streams
2. **Advanced RIST Features**: Implement encryption, authentication, and advanced recovery
3. **Dynamic Configuration**: Runtime RIST parameter adjustment
4. **Monitoring Integration**: RIST-specific metrics and health monitoring

### Client Integration

Future work should focus on creating a client-side `RistTransport` implementation that mirrors the server architecture for consistency and maintainability.

## Conclusion

The current RIST implementation represents a significant architectural improvement over initial attempts. By embracing RIST's native design principles and following the proven testrist pattern, we've created a clean, maintainable, and extensible foundation for reliable internet streaming in Snapcast.

The key lesson learned: **don't fight the protocol's nature** - embrace RIST's strengths rather than trying to make it behave like TCP. This alignment between code structure and protocol design results in simpler, more reliable software.