The `ClientConnectionRistBidirectional` implementation has several issues that could prevent the `ristDataCallback` from triggering or cause other problems. Below are the identified issues and their fixes, focusing on aligning with the TCP/WebSocket implementations and addressing the bidirectional RIST setup.

### Issues and Fixes

1. **Port Conflict in Client Configuration**
   - **Problem**: The client configures its sender to connect to `server_.port + 1` (e.g., 1707), but the server (from your previous `StreamSessionRistBidirectional`) uses `client_port_ + 1` (1707) for its receiver’s RTP port, which conflicts with its sender’s RTCP port (also 1707). As noted previously, the server’s receiver should use `client_port_ + 2` (e.g., 1708) to avoid RTCP overlap. The client’s sender must connect to this corrected port (1708).
   - **Fix**: Update the client’s sender URL to use `server_.port + 2`:
     ```cpp
     uint16_t backchannel_port = server_.port + 2; // Match server’s receiver (e.g., 1708)
     std::string sender_url = "rist://" + server_.host + ":" + std::to_string(backchannel_port);
     LOG(INFO, LOG_TAG) << "Using RIST sender URL: " << sender_url << "\n";
     ```

2. **Incorrect Logging Callback Assignment**
   - **Problem**: In `initRist()`, the sender context creation incorrectly assigns `log_settings_receiver.log_cb_arg` instead of `log_settings_sender.log_cb_arg`. This causes the sender’s logs to use the receiver’s context string (" receiver "), leading to confusing log output.
   - **Fix**: Correct the sender’s logging settings:
     ```cpp
     // Create RIST sender context for sending backchannel to server
     rist_logging_settings log_settings_sender = log_settings_;
     log_settings_sender.log_cb_arg = static_cast<void*>(const_cast<char*>(" sender ")); // Fixed
     ret = rist_sender_create(&sender_ctx_, RIST_PROFILE_MAIN, 0, &log_settings_sender);
     ```

3. **Missing `shared_from_this` in Constructor**
   - **Problem**: The TCP/WebSocket implementations use `shared_from_this` to ensure the session object remains alive during async operations. The RIST client doesn’t set a `self_` member (unlike the server’s `StreamSessionRistBidirectional`), which could lead to object lifetime issues, especially in callbacks.
   - **Fix**: Add a `self_` member and set it in the constructor:
     ```cpp
     class ClientConnectionRistBidirectional : public ClientConnection {
     private:
         std::shared_ptr<ClientConnectionRistBidirectional> self_;
         // ... other members
     };

     ClientConnectionRistBidirectional::ClientConnectionRistBidirectional(boost::asio::io_context& io_context, ClientSettings::Server server)
         : ClientConnection(io_context, std::move(server)) {
         self_ = std::static_pointer_cast<ClientConnectionRistBidirectional>(shared_from_this());
         LOG(INFO, LOG_TAG) << "Creating bidirectional RIST client connection\n";
         buffer_.resize(8192);
     }
     ```

4. **Pending Handler Management in `getNextMessage`**
   - **Problem**: The `getNextMessage` method stores the handler in `pending_handler_` but doesn’t initiate any read operation, unlike the TCP/WebSocket implementations, which call `async_read`. Since RIST uses a callback-driven model (`ristDataCallback`), `getNextMessage` should trigger the next message processing or ensure the callback is active.
   - **Fix**: Since `ristDataCallback` handles data reception, modify `getNextMessage` to ensure the callback is set and the message queue is processed:
     ```cpp
     void ClientConnectionRistBidirectional::getNextMessage(const MessageHandler<msg::BaseMessage>& handler) {
         std::lock_guard<std::mutex> lock(handler_mutex_);
         pending_handler_ = handler;
         LOG(DEBUG, LOG_TAG) << "Registered message handler, waiting for RIST data callback\n";
         // Ensure callback is active (already set in initRist)
         queue_cv_.notify_one(); // Wake up message processor thread
     }
     ```

5. **Message Processing Not Matching TCP/WebSocket**
   - **Problem**: The TCP/WebSocket `getNextMessage` implementations perform two-stage reading (header then body) and directly call `messageReceived` with the deserialized message. The RIST `messageProcessorThread` does similar processing but may skip messages if `pending_handler_` is unset. Additionally, it doesn’t consume the buffer correctly, unlike `buffer_.consume(bytes_transferred)` in WebSocket.
   - **Fix**: Update `messageProcessorThread` to match the TCP/WebSocket flow and ensure buffer management:
     ```cpp
     void ClientConnectionRistBidirectional::messageProcessorThread() {
         LOG(INFO, LOG_TAG) << "Starting bidirectional RIST receiver thread\n";
         while (running_) {
             QueuedMessage msg;
             {
                 std::unique_lock<std::mutex> lock(queue_mutex_);
                 queue_cv_.wait(lock, [this] { return !message_queue_.empty() || !running_; });
                 if (!running_) break;
                 if (message_queue_.empty()) continue;
                 msg = std::move(message_queue_.front());
                 message_queue_.pop();
             }

             try {
                 if (msg.virt_port == VPORT_AUDIO || msg.virt_port == VPORT_CONTROL) {
                     std::lock_guard<std::mutex> lock(buffer_mutex_);
                     if (buffer_.size() < msg.data.size()) {
                         buffer_.resize(msg.data.size());
                     }
                     memcpy(buffer_.data(), msg.data.data(), msg.data.size());

                     if (msg.data.size() >= base_msg_size_) {
                         base_message_.deserialize(reinterpret_cast<char*>(buffer_.data()));
                         LOG(DEBUG, LOG_TAG) << "Parsed message header: type=" << base_message_.type 
                                            << ", size=" << base_message_.size << ", id=" << base_message_.id 
                                            << ", vport=" << msg.virt_port << "\n";

                         if (base_message_.type > message_type::kLast) {
                             LOG(ERROR, LOG_TAG) << "Unknown message type: " << base_message_.type << "\n";
                         } else if (base_message_.size > msg::max_size) {
                             LOG(ERROR, LOG_TAG) << "Message too large: " << base_message_.size << "\n";
                         } else if (base_message_.size <= msg.data.size()) {
                             auto message = msg::factory::createMessage(base_message_, 
                                 reinterpret_cast<char*>(buffer_.data()) + base_msg_size_);
                             if (message) {
                                 tv now;
                                 base_message_.received = now;
                                 std::lock_guard<std::mutex> handler_lock(handler_mutex_);
                                 if (pending_handler_) {
                                     messageReceived(std::move(message), [this, h = pending_handler_](boost::system::error_code ec, std::unique_ptr<msg::BaseMessage> msg) {
                                         h(ec, std::move(msg));
                                         if (!ec) getNextMessage(h); // Chain next read like TCP/WebSocket
                                     });
                                     pending_handler_ = nullptr;
                                 }
                             } else {
                                 LOG(WARNING, LOG_TAG) << "Failed to create message from factory\n";
                             }
                         } else {
                             LOG(DEBUG, LOG_TAG) << "Incomplete message: expected " << base_message_.size 
                                                << ", got " << msg.data.size() << "\n";
                         }
                     } else {
                         LOG(DEBUG, LOG_TAG) << "Data too small for header: " << msg.data.size() << "\n";
                     }
                     buffer_.clear(); // Mimic WebSocket buffer_.consume
                 }
             } catch (const std::exception& e) {
                 LOG(ERROR, LOG_TAG) << "Exception in message processor: " << e.what() << "\n";
             }
         }
         LOG(INFO, LOG_TAG) << "Bidirectional RIST receiver thread stopped\n";
     }
     ```

6. **Connection Timeout Handling**
   - **Problem**: The `doConnect` method waits up to 5 seconds for `connected_` to be set, but it doesn’t handle partial connections (e.g., receiver connects but sender fails). This could lead to premature timeouts.
   - **Fix**: Log connection status details during the wait and check both `sender_connected_` and `receiver_connected_`:
     ```cpp
     while (!connected_ && std::chrono::steady_clock::now() < timeout) {
         LOG(DEBUG, LOG_TAG) << "Waiting for RIST connections: sender=" << sender_connected_ 
                            << ", receiver=" << receiver_connected_ << "\n";
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
     }
     ```

7. **Server-Side Port Mismatch**
   - **Problem**: The client’s sender connects to `server_.port + 1` (1707), but the server’s receiver (from `StreamSessionRistBidirectional`) should be on `client_port_ + 2` (1708). This mismatch was addressed in the server code previously, but ensure consistency.
   - **Fix**: Verify the server’s receiver is on `client_port_ + 2` (e.g., 1708), as corrected in the server implementation.

### Additional Recommendations

- **Debugging**:
  - Add libRIST stats callback to monitor packet flow:
    ```cpp
    rist_stats_callback_set(receiver_ctx_, 1000, [](void* arg, const struct rist_stats* stats) {
        LOG(DEBUG, LOG_TAG) << "RIST stats: " << stats->stats_json << "\n";
        return 0;
    }, nullptr);
    ```
  - Use `netstat -an | grep 170[6,8]` to confirm client and server ports are bound/connected.
  - Check firewall rules for UDP ports 1706, 1707, 1708, 1709.

- **Testing on Same Machine**:
  - Server: Sender on `rist://@127.0.0.1:1706`, receiver on `rist://@127.0.0.1:1708`.
  - Client: Receiver on `rist://127.0.0.1:1706`, sender on `rist://127.0.0.1:1708`.

- **Message Handler Chaining**: The TCP/WebSocket implementations chain `getNextMessage` after processing a message. The RIST implementation should do the same to ensure continuous message reception.

### Summary of Key Changes
- Update client sender to connect to `server_.port + 2` (e.g., 1708) to match server’s receiver.
- Fix logging callback assignment for sender context.
- Add `self_` member and set it in the constructor.
- Update `getNextMessage` to notify the message processor thread.
- Revise `messageProcessorThread` to clear the buffer and chain `getNextMessage` like TCP/WebSocket.
- Enhance connection timeout logging for better debugging.

These changes should resolve the issue of `ristDataCallback` not triggering and ensure the RIST client processes `ServerSettings` and `CodecHeader` messages correctly, matching the TCP/WebSocket behavior. If the issue persists, share libRIST logs and confirm server-side port configuration.
