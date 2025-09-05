# RIST Test Suite

Simple C-based RIST server and client to test the message flow that's failing in Snapcast.

## Purpose

This test mimics Snapcast's RIST bidirectional flow:
- **Server**: Binds to ports 1706 (sender) and 1708 (receiver) 
- **Client**: Connects to server ports for bidirectional communication
- **Virtual Ports**: 
  - 1000: Audio data (like CodecHeader, AudioChunk)
  - 2000: Control data (like ServerSettings, Time)
  - 3000: Backchannel (like Hello, Time requests)

## Expected Flow

1. **Client → Server**: Hello message (VPORT_BACKCHANNEL 3000)
2. **Server → Client**: ServerSettings (VPORT_CONTROL 2000)  
3. **Server → Client**: CodecHeader (VPORT_AUDIO 1000) ← **Critical missing message in Snapcast**
4. **Client → Server**: Time echo (VPORT_BACKCHANNEL 3000)
5. **Server → Client**: Continuous AudioChunks (VPORT_AUDIO 1000)

## Build

```bash
make all
```

## Run Test

### Manual (two terminals):
```bash
# Terminal 1
./server

# Terminal 2  
./client
```

### Automatic:
```bash
make run-test
```

## Key Test Points

1. **CodecHeader Delivery**: Does the client receive the CodecHeader on VPORT_AUDIO 1000?
2. **Virtual Port Multiplexing**: Are all virtual ports working correctly?
3. **Bidirectional Flow**: Can both sides send/receive simultaneously?
4. **Message Timing**: Any deadlocks or missing messages?

## Expected Output

If working correctly, you should see:
- Client receives Hello, ServerSettings, **CodecHeader**, Time responses
- Server receives Hello, Time echoes  
- Continuous audio chunks flowing
- No timeouts or "dead flow" messages

If the same issue as Snapcast occurs:
- CodecHeader will not be received by client
- Audio flow will fail to establish
- Connection will timeout due to no data flow