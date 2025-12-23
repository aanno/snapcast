Yes, there’s room to create a common header for shared constants like `VPORT_AUDIO`, `VPORT_CONTROL`, and `VPORT_BACKCHANNEL`, as well as shared functionality like the `rist_log_callback`. This reduces code duplication and ensures consistency between the client (`ClientConnectionRistBidirectional`) and server (`StreamSessionRistBidirectional`) implementations in Snapcast. Below, I’ll sketch a common header and identify opportunities for shared implementations.

### Common Header
A shared header can define virtual port constants and the logging callback, as these are identical across client and server. Other shared utilities (e.g., RIST context creation, cleanup, or callback helpers) can also be included.

<xaiArtifact artifact_id="f21a8b23-a99b-47a7-bc4f-69818770f57f" artifact_version_id="46e7115b-ce1e-45a6-8dac-57a5b8da0461" title="rist_common.hpp" contentType="text/x-c++hdr">
#pragma once

#include <librist/librist.h>
#include "common/aixlog.hpp"
#include <string>

namespace rist_common {

static constexpr auto LOG_TAG = "libRIST";

// Virtual port assignments for multiplexing
static constexpr uint16_t VPORT_AUDIO = 1000;       // Audio data
static constexpr uint16_t VPORT_CONTROL = 2000;     // Control messages
static constexpr uint16_t VPORT_BACKCHANNEL = 3000; // Backchannel data

// Thread-safe RIST logging callback
static int rist_log_callback(void* arg, enum rist_log_level level, const char* msg) {
    const char* context = static_cast<char*>(arg);
    switch (level) {
        case RIST_LOG_ERROR:
            LOG(ERROR, LOG_TAG) << context << msg << "\n";
            break;
        case RIST_LOG_WARN:
            LOG(WARNING, LOG_TAG) << context << msg << "\n";
            break;
        case RIST_LOG_INFO:
            LOG(INFO, LOG_TAG) << context << msg << "\n";
            break;
        case RIST_LOG_DEBUG:
            LOG(DEBUG, LOG_TAG) << context << msg << "\n";
            break;
        default:
            LOG(DEBUG, LOG_TAG) << context << msg << "\n";
            break;
    }
    return 0;
}

// Initialize global RIST logging
inline bool init_rist_logging(struct rist_logging_settings& log_settings, const char* context = " global ") {
    log_settings = {};
    log_settings.log_level = RIST_LOG_DEBUG;
    log_settings.log_stream = nullptr; // stdout disabled to avoid duplication
    log_settings.log_cb = rist_log_callback;
    log_settings.log_cb_arg = const_cast<char*>(context);
    return rist_logging_set_global(&log_settings) == 0;
}

} // namespace rist_common
</xaiArtifact>

### Shared Implementations
Beyond the header, some functionality can be shared to reduce duplication:

1. **RIST Context Creation and Cleanup**:
   - Both client and server create sender and receiver contexts with similar steps (create, set callbacks, parse URLs, start). A templated or base class approach could abstract this.
   - Example shared utility for context creation:
     ```cpp
     namespace rist_common {
     template <typename ContextType>
     bool create_rist_context(ContextType*& ctx, enum rist_profile profile, uint16_t flags, 
                             const std::string& url, struct rist_peer*& peer, 
                             const char* log_context, const char* log_tag) {
         struct rist_logging_settings log_settings;
         if (!init_rist_logging(log_settings, log_context)) {
             LOG(ERROR, log_tag) << "Failed to initialize RIST logging\n";
             return false;
         }
         int ret = (std::is_same_v<ContextType, rist_ctx> ? 
                    rist_sender_create(&ctx, profile, flags, &log_settings) : 
                    rist_receiver_create(&ctx, profile, &log_settings));
         if (ret != 0) {
             LOG(ERROR, log_tag) << "Failed to create RIST context: " << ret << "\n";
             return false;
         }
         struct rist_peer_config* config = nullptr;
         ret = rist_parse_address2(url.c_str(), &config);
         if (ret < 0) {
             LOG(ERROR, log_tag) << "Failed to parse RIST URL: " << url << ", error: " << ret << "\n";
             rist_destroy(ctx);
             ctx = nullptr;
             return false;
         }
         ret = rist_peer_create(ctx, &peer, config);
         free(config);
         if (ret != 0) {
             LOG(ERROR, log_tag) << "Failed to create RIST peer: " << ret << "\n";
             rist_destroy(ctx);
             ctx = nullptr;
             return false;
         }
         ret = rist_start(ctx);
         if (ret != 0) {
             LOG(ERROR, log_tag) << "Failed to start RIST context: " << ret << "\n";
             rist_destroy(ctx);
             ctx = nullptr;
             peer = nullptr;
             return false;
         }
         return true;
     }

     inline void cleanup_rist_context(rist_ctx*& ctx, rist_peer*& peer, const char* log_tag) {
         if (ctx) {
             LOG(DEBUG, log_tag) << "Cleaning up RIST context\n";
             rist_destroy(ctx);
             ctx = nullptr;
             peer = nullptr;
         }
     }
     }
     ```
   - Usage in server:
     ```cpp
     if (!rist_common::create_rist_context(sender_ctx_, RIST_PROFILE_MAIN, 0, 
                                          "rist://@0.0.0.0:" + std::to_string(client_port_), 
                                          sender_peer_, " sender ", LOG_TAG)) {
         return false;
     }
     if (!rist_common::create_rist_context(receiver_ctx_, RIST_PROFILE_MAIN, 0, 
                                          "rist://@0.0.0.0:" + std::to_string(client_port_ + 2), 
                                          receiver_peer_, " receiver ", LOG_TAG)) {
         cleanupRist();
         return false;
     }
     ```
   - Usage in client:
     ```cpp
     if (!rist_common::create_rist_context(receiver_ctx_, RIST_PROFILE_MAIN, 0, 
                                          "rist://" + server_.host + ":" + std::to_string(server_.port), 
                                          receiver_peer_, " receiver ", LOG_TAG)) {
         return false;
     }
     if (!rist_common::create_rist_context(sender_ctx_, RIST_PROFILE_MAIN, 0, 
                                          "rist://" + server_.host + ":" + std::to_string(server_.port + 2), 
                                          sender_peer_, " sender ", LOG_TAG)) {
         cleanupRist();
         return false;
     }
     ```

2. **Connection Status Callback**:
   - The `senderConnectionStatusCallback` and `receiverConnectionStatusCallback` are nearly identical in both implementations. A shared function can handle status updates:
     ```cpp
     namespace rist_common {
     template <typename SessionType>
     void connection_status_callback(void* arg, struct rist_peer* /*peer*/, enum rist_connection_status status, 
                                    bool& connected_flag, bool& overall_connected, 
                                    const char* log_tag, StreamMessageReceiver* receiver = nullptr) {
         auto* session = static_cast<SessionType*>(arg);
         if (!session) return;
         switch (status) {
             case RIST_CONNECTION_ESTABLISHED:
             case RIST_CLIENT_CONNECTED:
                 LOG(INFO, log_tag) << "RIST connection established\n";
                 connected_flag = true;
                 overall_connected = session->sender_connected_ && session->receiver_connected_;
                 break;
             case RIST_CONNECTION_TIMED_OUT:
             case RIST_CLIENT_TIMED_OUT:
                 LOG(WARNING, log_tag) << "RIST connection timed out\n";
                 connected_flag = false;
                 overall_connected = false;
                 if (receiver) receiver->onDisconnect(session);
                 break;
             default:
                 LOG(WARNING, log_tag) << "Unknown RIST connection status: " << status << "\n";
                 break;
         }
     }
     }
     ```
   - Usage in server/client:
     ```cpp
     rist_connection_status_callback_set(sender_ctx_, 
         [](void* arg, struct rist_peer* peer, enum rist_connection_status status) {
             rist_common::connection_status_callback<StreamSessionRistBidirectional>(
                 arg, peer, status, sender_connected_, connected_, LOG_TAG, messageReceiver_);
         }, this);
     ```

3. **Message Processing**:
   - The `messageProcessorThread` is similar in both implementations. A base class or shared utility could handle the message queue and deserialization, but since the server and client have different base classes (`StreamSession` vs. `ClientConnection`), a shared utility function is more practical:
     ```cpp
     namespace rist_common {
     template <typename SessionType>
     void process_queued_message(SessionType* session, QueuedMessage& msg, std::vector<char>& buffer, 
                                msg::BaseMessage& base_message, size_t base_msg_size, 
                                const char* log_tag, StreamMessageReceiver* receiver = nullptr) {
         try {
             std::lock_guard<std::mutex> lock(session->buffer_mutex_);
             if (buffer.size() < msg.data.size()) {
                 buffer.resize(msg.data.size());
             }
             memcpy(buffer.data(), msg.data.data(), msg.data.size());
             if (msg.data.size() >= base_msg_size) {
                 base_message.deserialize(reinterpret_cast<char*>(buffer.data()));
                 LOG(DEBUG, log_tag) << "Parsed message: type=" << base_message.type 
                                    << ", size=" << base_message.size << ", vport=" << msg.virt_port << "\n";
                 if (base_message.type > msg::message_type::kLast) {
                     LOG(ERROR, log_tag) << "Unknown message type: " << base_message.type << "\n";
                 } else if (base_message.size > msg::max_size) {
                     LOG(ERROR, log_tag) << "Message too large: " << base_message.size << "\n";
                 } else if (base_message.size <= msg.data.size()) {
                     auto message = msg::factory::createMessage(base_message, 
                         reinterpret_cast<char*>(buffer.data()) + base_msg_size);
                     if (message && receiver && session->self_) {
                         tv now;
                         base_message.received = now;
                         receiver->onMessageReceived(session->self_, base_message, 
                             reinterpret_cast<char*>(buffer.data()) + base_msg_size);
                     } else {
                         LOG(WARNING, log_tag) << "Failed to process message\n";
                     }
                 } else {
                     LOG(DEBUG, log_tag) << "Incomplete message: expected " << base_message.size 
                                        << ", got " << msg.data.size() << "\n";
                 }
                 buffer.clear();
             } else {
                 LOG(DEBUG, log_tag) << "Data too small for header: " << msg.data.size() << "\n";
             }
         } catch (const std::exception& e) {
             LOG(ERROR, log_tag) << "Exception in message processor: " << e.what() << "\n";
         }
     }
     }
     ```

### Integration
- **Header Usage**: Include `rist_common.hpp` in both `stream_session_rist_bidirectional.hpp` and `client_connection_rist_bidirectional.hpp`. Replace `VPORT_*` constants and `rist_log_callback` with `rist_common::VPORT_*` and `rist_common::rist_log_callback`.
- **Shared Functions**: Use `create_rist_context`, `cleanup_rist_context`, `connection_status_callback`, and `process_queued_message` in both implementations to reduce duplication.
- **Client-Specific Changes**: Update `ClientConnectionRistBidirectional::messageProcessorThread` to use `rist_common::process_queued_message` and handle `pending_handler_` for chaining `getNextMessage`, as shown in the previous response.
- **Server-Specific Changes**: Update `StreamSessionRistBidirectional::messageProcessorThread` to use `rist_common::process_queued_message`, ensuring it processes all virtual ports (1000, 2000, 3000).

### Benefits
- **Consistency**: Shared constants ensure virtual ports match across client and server.
- **Maintainability**: Centralized logging and context management reduce code duplication.
- **Extensibility**: Shared utilities can be reused for future RIST-based features.

If you want a full implementation of the updated client or server code using these shared utilities, let me know!
