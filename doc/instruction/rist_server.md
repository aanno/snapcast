Your libRIST implementation for `StreamSessionRistBidirectional` is mostly correct but has a few issues that could explain why the client's `ristDataCallback` is not being triggered. Below are the identified errors and suggested fixes, focusing on the core issues:

### Issues and Fixes

1. **RTCP Port Conflict (Server Side)**
   - **Problem**: The server uses `client_port_` for the sender (RTP 1706, RTCP 1707) and `client_port_ + 1` for the receiver (RTP 1707, RTCP 1708), causing an RTCP port conflict on 1707. This can prevent proper connection establishment or data flow.
   - **Fix**: Use non-overlapping ports. Set the receiver to `client_port_ + 2` (e.g., RTP 1708, RTCP 1709).
   - **Code Change**:
     ```cpp
     uint16_t backchannel_port = client_port_ + 2; // Use +2 to avoid RTCP conflict
     std::string receiver_url = "rist://@0.0.0.0:" + std::to_string(backchannel_port);
     ```

2. **Client Configuration Mismatch**
   - **Problem**: The client must connect its receiver to the server’s sender port (e.g., 1706) and its sender to the server’s receiver port (e.g., 1708). Your code doesn’t show the client-side configuration, but if it’s connecting to the wrong ports (e.g., both to 1706 or 1707), the receiver callback won’t trigger.
   - **Fix**: Ensure the client configures:
     - Receiver: `rist://server_ip:1706`
     - Sender: `rist://server_ip:1708`
   - **Verification**: Add client-side logs to confirm the URLs used.

3. **Callback Not Triggering**
   - **Problem**: The `ristDataCallback` is not being called, despite connections being established. This could be due to:
     - Incorrect virtual port handling on the client side (not shown in your code).
     - Data not being received due to port misconfiguration or firewall issues.
     - The server not sending data to the correct client address/port.
   - **Fix**:
     - Verify the client’s receiver context is set up with `rist_receiver_data_callback_set2` and points to the server’s sender port (1706).
     - Ensure the server’s sender peer is correctly configured to send to the client’s address/port (client receiver’s RTP port).
     - Add debug logs in the callback to confirm invocation:
       ```cpp
       int StreamSessionRistBidirectional::ristDataCallback(void* arg, struct rist_data_block* data_block) {
           auto* session = static_cast<StreamSessionRistBidirectional*>(arg);
           if (!session || !data_block) {
               LOG(ERROR, LOG_TAG) << "Invalid callback args or data block\n";
               return 0;
           }
           LOG(DEBUG, LOG_TAG) << "Data callback triggered: " << data_block->payload_len 
                              << " bytes on vport " << data_block->virt_dst_port << "\n";
           // Rest of your callback code
       }
       ```

4. **Virtual Port Mismatch**
   - **Problem**: The server sends `ServerSettings` on virtual port 2000 and `CodecHeader` on 1000, but the client’s `ristDataCallback` only processes `VPORT_BACKCHANNEL` (3000). This means server-to-client messages (1000, 2000) are ignored.
   - **Fix**: Update the client’s callback to handle all relevant virtual ports (1000, 2000, 3000). For server-to-client messages, process them similarly to the WebSocket implementation:
     ```cpp
     int StreamSessionRistBidirectional::ristDataCallback(void* arg, struct rist_data_block* data_block) {
         auto* session = static_cast<StreamSessionRistBidirectional*>(arg);
         if (!session || !data_block) {
             LOG(ERROR, LOG_TAG) << "Invalid callback args or data block\n";
             return 0;
         }

         LOG(DEBUG, LOG_TAG) << "Data callback: " << data_block->payload_len 
                            << " bytes on vport " << data_block->virt_dst_port << "\n";

         // Handle all virtual ports
         if (data_block->virt_dst_port == VPORT_AUDIO || 
             data_block->virt_dst_port == VPORT_CONTROL || 
             data_block->virt_dst_port == VPORT_BACKCHANNEL) {
             QueuedMessage msg;
             msg.data.resize(data_block->payload_len);
             memcpy(msg.data.data(), data_block->payload, data_block->payload_len);
             msg.virt_port = data_block->virt_dst_port;

             {
                 std::lock_guard<std::mutex> lock(session->queue_mutex_);
                 session->message_queue_.push(std::move(msg));
             }
             session->queue_cv_.notify_one();
         } else {
             LOG(WARNING, LOG_TAG) << "Unknown virtual port: " << data_block->virt_dst_port << "\n";
         }
         return 0;
     }
     ```

5. **Message Processing Inconsistency**
   - **Problem**: The `messageProcessorThread` checks for `VPORT_BACKCHANNEL` before processing, which skips server-to-client messages (e.g., `ServerSettings`, `CodecHeader`). This doesn’t match the WebSocket implementation, where all messages are processed via `onMessageReceived`.
   - **Fix**: Remove the `VPORT_BACKCHANNEL` check in `messageProcessorThread` to process all messages, mirroring the WebSocket approach:
     ```cpp
     void StreamSessionRistBidirectional::messageProcessorThread() {
         LOG(INFO, LOG_TAG) << "Starting RIST message processor thread\n";
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
                 std::lock_guard<std::mutex> lock(buffer_mutex_);
                 if (buffer_.size() < msg.data.size()) {
                     buffer_.resize(msg.data.size());
                 }
                 memcpy(buffer_.data(), msg.data.data(), msg.data.size());

                 if (msg.data.size() >= base_msg_size_) {
                     std::vector<char> header_buffer(base_msg_size_);
                     memcpy(header_buffer.data(), buffer_.data(), base_msg_size_);
                     baseMessage_.deserialize(header_buffer.data());

                     LOG(DEBUG, LOG_TAG) << "Parsed message: type=" << baseMessage_.type 
                                        << ", size=" << baseMessage_.size << ", id=" << baseMessage_.id 
                                        << ", vport=" << msg.virt_port << "\n";

                     if (baseMessage_.type > message_type::kLast) {
                         LOG(ERROR, LOG_TAG) << "Unknown message type: " << baseMessage_.type << "\n";
                     } else if (baseMessage_.size > msg::max_size) {
                         LOG(ERROR, LOG_TAG) << "Message too large: " << baseMessage_.size << "\n";
                     } else if (baseMessage_.size <= msg.data.size()) {
                         if (messageReceiver_ && self_) {
                             tv now;
                             baseMessage_.received = now;
                             messageReceiver_->onMessageReceived(self_, baseMessage_, 
                                 reinterpret_cast<char*>(buffer_.data()) + base_msg_size_);
                         }
                     } else {
                         LOG(DEBUG, LOG_TAG) << "Incomplete message: expected " << baseMessage_.size 
                                            << ", got " << msg.data.size() << "\n";
                     }
                 } else {
                     LOG(DEBUG, LOG_TAG) << "Data too small for header: " << msg.data.size() << "\n";
                 }
             } catch (const std::exception& e) {
                 LOG(ERROR, LOG_TAG) << "Exception in message processor: " << e.what() << "\n";
             }
         }
         LOG(INFO, LOG_TAG) << "RIST message processor thread stopped\n";
     }
     ```

6. **Logging Context Lifetime**
   - **Problem**: The `log_settings_.log_cb_arg` uses a string literal (" global "), but `log_settings_sender` and `log_settings_receiver` copy it with new string literals (" sender ", " receiver "). These are safe, but ensure the `arg` pointer remains valid during the context’s lifetime.
   - **Fix**: No change needed if string literals are used, as they have static storage duration. For dynamic strings, ensure proper memory management.

7. **Self-Reference Handling**
   - **Problem**: The WebSocket implementation uses `shared_from_this()` in `sendAsync` and `do_read_ws` to ensure the session object remains alive. Your RIST implementation sets `self_` but doesn’t consistently use it in `sendAsync` or `ristDataCallback`.
   - **Fix**: Ensure `self_` is set in the constructor and used in `sendAsync`:
     ```cpp
     StreamSessionRistBidirectional::StreamSessionRistBidirectional(...) : ... {
         self_ = shared_from_this();
         // Rest of constructor
     }

     void StreamSessionRistBidirectional::sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler) {
         if (!sender_ctx_ || !sender_connected_) {
             LOG(WARNING, LOG_TAG) << "Cannot send data - RIST sender not connected\n";
             if (handler) {
                 handler(boost::system::error_code(boost::asio::error::not_connected), 0);
             }
             return;
         }
         auto self = self_; // Keep session alive
         // Rest of sendAsync code
     }
     ```

### Additional Recommendations

- **Debugging**:
  - Add logs to confirm client receiver URL and virtual port handling.
  - Use `netstat -an | grep 170[6,8]` on the server to verify ports are bound correctly.
  - Check firewall rules to ensure UDP ports (1706, 1708, 1709) are open.
  - Enable libRIST stats callback (`rist_stats_callback_set`) to monitor packet flow.

- **Client-Side Code**: Share the client-side implementation to verify its receiver context and callback setup.

- **Testing on Same Machine**:
  - Use `127.0.0.1` for both server and client.
  - Example client config:
    ```cpp
    std::string receiver_url = "rist://127.0.0.1:" + std::to_string(server_port); // 1706
    std::string sender_url = "rist://127.0.0.1:" + std::to_string(server_port + 2); // 1708
    ```

### Summary of Key Changes
- Fix port conflict: Server receiver to `client_port_ + 2` (e.g., 1708).
- Update client `ristDataCallback` to handle all virtual ports (1000, 2000, 3000).
- Remove `VPORT_BACKCHANNEL` check in `messageProcessorThread` to process all messages.
- Ensure client connects to correct server ports (1706 for receiver, 1708 for sender).
- Add debug logs in `ristDataCallback` to confirm invocation.
- Use `self_` consistently to match WebSocket’s `shared_from_this()` pattern.

These changes should ensure the client’s `ristDataCallback` triggers for `ServerSettings` and `CodecHeader` messages, aligning with the WebSocket behavior. If issues persist, share client-side code and libRIST logs for further analysis.
