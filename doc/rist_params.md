# RIST parameters

You're correct, libRIST v0.2.7 allows bufmin and bufmax configuration via rist_peer_config (seen in rist-common.c, ristreceiver.c, ristsender.c). The logs show bufmin=1000, bufmax=1000 (ms) as defaults. Override these in your rist_receiver_set_config call (e.g., set bufmin=50, bufmax=50) to reduce buffering delay. Ensure fifo_size is a power of 2 (e.g., 64ms) to avoid the "Desired fifo size must be a power of 2" error (snapclient.log: 19-19-15.410). Extend client hello timeout to >3 seconds as a fallback. Test and verify reduced latency.

## FIFO Size

`ret = rist_receiver_set_output_fifo_size(receiver_ctx_, 50); // 50ms FIFO - balanced approach`

## Polling

`int ret = rist_receiver_data_read2(receiver_ctx_, &data_block, 10); // 10ms timeout - matches FIFO stability`

Polling works better due to inappropriate RIST parameters (bufmin=1000, bufmax=1000 causing ~2s delay). Callback isn't necessarily broken in libRIST v0.2.7, but polling avoids potential callback issues (e.g., race conditions seen earlier). Set bufmin=50, bufmax=50, and fifo_size to a power of 2 (e.g., 64ms) in rist_receiver_set_config. Extend client hello timeout to >3s. Test callback with optimized parameters to confirm functionality.

## bufmin/bufmax and other (from stats)

librist/src/rist-common.c, line 288:

```cpp
		rist_log_priv(get_cctx(peer), RIST_LOG_INFO,
				"New peer with id #%"PRIu32" was configured with maxrate=%d/%d bufmin=%d bufmax=%d reorder=%d rttmin=%d rttmax=%d congestion_control=%d min_retries=%d max_retries=%d\n",
				peer->adv_peer_id, peer->config.recovery_maxbitrate, peer->config.recovery_maxbitrate_return, peer->config.recovery_length_min, peer->config.recovery_length_max, peer->config.recovery_reorder_buffer,
				peer->config.recovery_rtt_min /RIST_CLOCK, peer->config.recovery_rtt_max /RIST_CLOCK, peer->config.congestion_control_mode, peer->config.min_retries, peer->config.max_retries);
```

* id: adv_peer_id
* maxrate: recovery_maxbitrate
* bufmin: recovery_length_min
* bufmax: recovery_length_max
* reorder: recovery_reorder_buffer
* rttmin: recovery_rtt_min
* rttmax: recovery_rtt_max
* congestion_control: congestion_control_mode
* min_retries: min_retries
* max_retries: max_retries

librist/tools/ristreceiver.c, line 826:

```cpp
		rist_log(&logging_settings, RIST_LOG_INFO, "Link configured with maxrate=%d bufmin=%d bufmax=%d reorder=%d rttmin=%d rttmax=%d congestion_control=%d min_retries=%d max_retries=%d\n",
			peer_config->recovery_maxbitrate, peer_config->recovery_length_min, peer_config->recovery_length_max,
			peer_config->recovery_reorder_buffer, peer_config->recovery_rtt_min,peer_config->recovery_rtt_max,
			peer_config->congestion_control_mode, peer_config->min_retries, peer_config->max_retries);
```

* maxrate: recovery_maxbitrate
* bufmin: recovery_length_min
* bufmax: recovery_length_max
* reorder: recovery_reorder_buffer
* rttmin: recovery_rtt_min
* rttmax: recovery_rtt_max
* congestion_control: congestion_control_mode
* min_retries: min_retries
* max_retries: max_retries

librist/tools/ristsender.c, line 514:

```cpp
	rist_log(&logging_settings, RIST_LOG_INFO, "Link configured with maxrate=%d bufmin=%d bufmax=%d reorder=%d rttmin=%d rttmax=%d congestion_control=%d min_retries=%d max_retries=%d\n",
		peer_config_link->recovery_maxbitrate, peer_config_link->recovery_length_min, peer_config_link->recovery_length_max,
		peer_config_link->recovery_reorder_buffer, peer_config_link->recovery_rtt_min, peer_config_link->recovery_rtt_max,
		peer_config_link->congestion_control_mode, peer_config_link->min_retries, peer_config_link->max_retries);
```

* maxrate: recovery_maxbitrate
* bufmin: recovery_length_min
* bufmax: recovery_length_max
* reorder: recovery_reorder_buffer
* rttmin: recovery_rtt_min
* rttmax: recovery_rtt_max
* congestion_control: congestion_control_mode
* min_retries: min_retries
* max_retries: max_retries  

## Callback

### Potential Callback Implementation

```cpp
#include <librist/librist.h>
#include <librist/udpsocket.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

// Global context and mutex for thread safety
struct rist_ctx *receiver_ctx;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

// Callback function for RIST data
void rist_data_callback(void *arg, struct rist_data_block *data_block) {
    if (!data_block || !data_block->payload || data_block->payload_len == 0) {
        return;
    }

    // Lock mutex to prevent race conditions
    pthread_mutex_lock(&buffer_mutex);

    // Process data based on virtual port
    if (data_block->virt_dst_port == 2000) { // Control port (ServerSettings)
        fprintf(stderr, "[Debug] (Callback) Received ServerSettings: %zu bytes on vport 2000\n", data_block->payload_len);
        // Parse and process ServerSettings
        // Example: process_server_settings(data_block->payload, data_block->payload_len);
    } else if (data_block->virt_dst_port == 1000) { // Audio port (CodecHeader)
        fprintf(stderr, "[Debug] (Callback) Received CodecHeader: %zu bytes on vport 1000\n", data_block->payload_len);
        // Parse and process CodecHeader
        // Example: process_codec_header(data_block->payload, data_block->payload_len);
    }

    // Send acknowledgment or next message in protocol flow
    // Example: send_ack_or_stream_request();

    // Unlock mutex
    pthread_mutex_unlock(&buffer_mutex);

    // Free data block
    rist_receiver_data_block_free(&data_block);
}

int main() {
    // Initialize logging
    struct rist_logging_settings logging_settings = LOGGING_SETTINGS_INITIALIZER;
    rist_log_set_global(&logging_settings, RIST_LOG_INFO);

    // Initialize receiver context
    if (rist_receiver_create(&receiver_ctx, RIST_PROFILE_MAIN, &logging_settings) != 0) {
        fprintf(stderr, "[Error] Failed to create RIST receiver\n");
        return -1;
    }

    // Configure peer with low-latency settings
    struct rist_peer_config peer_config = RIST_PEER_CONFIG_INITIALIZER;
    peer_config.maxrate = 100000;
    peer_config.bufmin = 50; // Reduced to 50ms
    peer_config.bufmax = 50; // Reduced to 50ms
    peer_config.reorder_buffer = 25;
    peer_config.rttmin = 50;
    peer_config.rttmax = 500;
    peer_config.congestion_control = 1;
    peer_config.min_retries = 6;
    peer_config.max_retries = 20;
    strncpy(peer_config.url, "rist://192.168.10.139:1706", sizeof(peer_config.url) - 1);

    struct rist_peer *peer;
    if (rist_peer_create(receiver_ctx, &peer, &peer_config) != 0) {
        fprintf(stderr, "[Error] Failed to create RIST peer\n");
        rist_destroy(receiver_ctx);
        return -1;
    }

    // Set FIFO size to power of 2 (64ms)
    if (rist_receiver_set_output_fifo_size(receiver_ctx, 64) != 0) {
        fprintf(stderr, "[Error] Failed to set FIFO size\n");
        rist_destroy(receiver_ctx);
        return -1;
    }

    // Set data callback
    if (rist_receiver_data_callback_set2(receiver_ctx, rist_data_callback, NULL) != 0) {
        fprintf(stderr, "[Error] Failed to set data callback\n");
        rist_destroy(receiver_ctx);
        return -1;
    }

    // Start receiver
    if (rist_start(receiver_ctx) != 0) {
        fprintf(stderr, "[Error] Failed to start RIST receiver\n");
        rist_destroy(receiver_ctx);
        return -1;
    }

    // Main loop to keep program running
    while (1) {
        sleep(1); // Replace with proper event loop or termination condition
    }

    // Cleanup
    rist_destroy(receiver_ctx);
    pthread_mutex_destroy(&buffer_mutex);
    return 0;
}
```
