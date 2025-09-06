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

----

Now I see the stark contrast! The simple testrist implementation works because
   it's fundamentally different from Snapcast's approach:

  Key Differences

  1. Session Lifecycle

  - testrist: Creates RIST contexts once, uses them throughout lifetime
  - Snapcast: Creates sessions dynamically per client connection

  2. Virtual Port Usage

  - testrist: Uses 3 distinct virtual ports (1000=audio, 2000=control,
  3000=backchannel)
  - Snapcast: Uses single bidirectional session with complex message routing

  3. Connection Model

  - testrist: Simple client connects to server ports 1706/1708
  - Snapcast: Complex session management with TCP-like accept/connect patterns

  4. Message Flow

  - testrist: Direct rist_sender_data_write() → immediate callback → response
  - Snapcast: Multi-threaded message queues → session management → complex
  routing

  5. Client Identity

  - testrist: No clientId concept - just point-to-point communication
  - Snapcast: Requires clientId for multi-client session management

● Root Cause Analysis

  The complexity arises because Snapcast is trying to force RIST into TCP's 
  connection model:

  1. TCP Model: Client connects → Server creates session → Unique clientId →
  Session lifecycle
  2. RIST Reality: Bidirectional channels → Virtual ports → Direct messaging →
  No inherent "connection" concept

  The Real Problem

  Snapcast's RIST implementation has a conceptual mismatch:

  - TCP: Connection-oriented, session per client
  - RIST: Message-oriented, virtual port multiplexing

  Simpler Solution Options

  1. Follow testrist model: Use virtual ports instead of session-per-client
  2. Fix session lifecycle: Don't add RIST sessions until Hello sets clientId
  3. Hybrid approach: Keep TCP-like interface but fix the session management

  The testrist works because it embraces RIST's nature rather than fighting it.
  Should we consider simplifying Snapcast's RIST implementation to follow the
  testrist pattern?

● The testrist implementation works because it's 120 lines vs 800+ lines and
  doesn't try to emulate TCP behavior over RIST!
