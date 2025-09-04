Yes, libRIST supports minimizing data copying (zero-copy) on both sender and receiver sides.

**Sender Side:**
- Use `rist_sender_data_write()` with a `rist_data_block` referencing your data directly.
- Set `payload` to point to your existing buffer, avoiding copies.
- Example:
  ```cpp
  rist_data_block block = {};
  block.payload = your_data; // Direct pointer to your buffer
  block.payload_len = data_size;
  block.virt_src_port = VPORT_AUDIO;
  block.virt_dst_port = VPORT_AUDIO;
  rist_sender_data_write(sender_ctx, &block);
  ```

**Receiver Side:**
- Use `rist_receiver_data_read2()` to get a `rist_data_block` with a direct pointer to the received data.
- Process `block->payload` directly without copying.
- Example:
  ```cpp
  rist_data_block *block;
  if (rist_receiver_data_read2(receiver_ctx, &block, timeout) > 0) {
    // Use block->payload directly
    process_data(block->payload, block->payload_len);
    rist_receiver_data_block_free(&block);
  }
  ```

**Key Points:**
- Avoid duplicating `block->payload` into another buffer.
- Ensure your buffer lifetimes align with libRIST's expectations (sender: until write completes; receiver: until `rist_receiver_data_block_free`).
- Use virtual ports for multiplexing as shown, no additional copying needed.

Yes, libRIST supports zero-copy on Linux network sockets using `sendmsg` for sending and `recvmsg` for receiving, leveraging kernel-level efficiency.

**Sender Side:**
- `rist_sender_data_write()` uses `sendmsg` internally, passing `rist_data_block->payload` directly to the socket without copying.
- Ensure your buffer is properly aligned and remains valid until the write completes.

**Receiver Side:**
- `rist_receiver_data_read2()` uses `recvmsg` to receive data directly into the `rist_data_block->payload` buffer, avoiding copies.
- Process `block->payload` directly and free with `rist_receiver_data_block_free`.

**Requirements:**
- Use Linux with a compatible kernel (modern versions support `sendmsg`/`recvmsg` zero-copy).
- Ensure buffers are allocated with sufficient alignment (e.g., `posix_memalign` for page-aligned memory).
- Avoid modifying or freeing buffers until libRIST operations complete.

**Note:**
- Zero-copy depends on the underlying socket implementation and kernel support. Verify with libRIST documentation or source for specific kernel requirements.
