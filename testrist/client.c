#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <librist/librist.h>

#define VPORT_AUDIO   1000
#define VPORT_CONTROL 2000
#define VPORT_BACKCHANNEL 3000

typedef struct {
    struct rist_ctx *sender_ctx;
    struct rist_ctx *receiver_ctx;
    int running;
    int server_connected;
    uint32_t msg_counter;
    uint32_t last_received_id;
} client_state_t;

static client_state_t g_client = {0};

// Message header structure (same as server)
typedef struct {
    uint16_t type;
    uint16_t id;
    uint32_t size;
    uint64_t timestamp;
} __attribute__((packed)) msg_header_t;

enum msg_type {
    MSG_HELLO = 1,
    MSG_SERVER_SETTINGS = 2,
    MSG_CODEC_HEADER = 3,
    MSG_TIME = 4,
    MSG_AUDIO_CHUNK = 5
};

void print_timestamp() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    printf("%02d:%02d:%02d.%03ld ", 
           (int)((ts.tv_sec % 86400) / 3600),
           (int)((ts.tv_sec % 3600) / 60),
           (int)(ts.tv_sec % 60),
           ts.tv_nsec / 1000000);
}

// RIST logging callback - simplified to avoid crashes
int rist_log_callback(void* arg, enum rist_log_level level, const char* msg) {
    // Avoid complex operations in callback - just return success
    (void)arg;    // suppress unused parameter warning
    (void)level;  // suppress unused parameter warning  
    (void)msg;    // suppress unused parameter warning
    return 0;
}

void send_message(int vport, uint16_t msg_type, uint32_t data_size, const void* data) {
    msg_header_t header = {0};
    header.type = msg_type;
    header.id = ++g_client.msg_counter;
    header.size = data_size;
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    header.timestamp = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    
    // Total message size
    uint32_t total_size = sizeof(header) + data_size;
    uint8_t* buffer = malloc(total_size);
    
    memcpy(buffer, &header, sizeof(header));
    if (data && data_size > 0) {
        memcpy(buffer + sizeof(header), data, data_size);
    }
    
    // Create rist_data_block structure
    struct rist_data_block data_block = {0};
    data_block.payload = buffer;
    data_block.payload_len = total_size;
    data_block.virt_dst_port = vport;
    data_block.ts_ntp = header.timestamp;
    
    int ret = rist_sender_data_write(g_client.sender_ctx, &data_block);
    print_timestamp();
    printf("[CLIENT] Sent msg_type=%d, id=%d, size=%d bytes to vport %d (ret=%d)\n",
           msg_type, header.id, total_size, vport, ret);
    
    free(buffer);
}

void send_hello() {
    char hello_data[200];
    snprintf(hello_data, sizeof(hello_data), "HELLO from test client - message #%d", g_client.msg_counter + 1);
    send_message(VPORT_BACKCHANNEL, MSG_HELLO, strlen(hello_data) + 1, hello_data);
}

void send_time_request() {
    uint64_t time_data = g_client.msg_counter + 3000;
    send_message(VPORT_BACKCHANNEL, MSG_TIME, sizeof(time_data), &time_data);
}

void echo_last_number() {
    // Echo the last received message ID back to server
    if (g_client.last_received_id > 0) {
        uint32_t echo_data = g_client.last_received_id;
        send_message(VPORT_BACKCHANNEL, MSG_TIME, sizeof(echo_data), &echo_data);
        print_timestamp();
        printf("[CLIENT] Echoed last received ID: %d\n", echo_data);
    }
}

int data_callback(void *arg, struct rist_data_block *data_block) {
    print_timestamp();
    printf("[CLIENT] Received %zu bytes on vport %d\n", data_block->payload_len, data_block->virt_dst_port);
    
    if (data_block->payload_len >= sizeof(msg_header_t)) {
        msg_header_t* header = (msg_header_t*)data_block->payload;
        print_timestamp();
        printf("[CLIENT] Message: type=%d, id=%d, size=%d\n", header->type, header->id, header->size);
        
        g_client.last_received_id = header->id;
        
        switch (header->type) {
            case MSG_SERVER_SETTINGS:
                print_timestamp();
                printf("[CLIENT] Received ServerSettings\n");
                if (header->size == sizeof(uint32_t)) {
                    uint32_t* settings = (uint32_t*)(data_block->payload + sizeof(msg_header_t));
                    printf("[CLIENT] Settings data: %d\n", *settings);
                }
                break;
                
            case MSG_CODEC_HEADER:
                print_timestamp();
                printf("[CLIENT] Received CodecHeader (%d bytes) - CRITICAL MESSAGE!\n", header->size);
                // This is the message that was missing in Snapcast RIST logs
                break;
                
            case MSG_TIME:
                print_timestamp();
                printf("[CLIENT] Received Time response\n");
                break;
                
            case MSG_AUDIO_CHUNK:
                print_timestamp();
                printf("[CLIENT] Received Audio chunk (%d bytes)\n", header->size);
                break;
                
            default:
                print_timestamp();
                printf("[CLIENT] Unknown message type: %d\n", header->type);
                break;
        }
    }
    
    return 0;
}

int main() {
    printf("=== RIST Test Client ===\n");
    
    // Wait a moment for server to start
    sleep(1);
    
    // Skip logging setup entirely to test
    printf("Skipping RIST logging setup to isolate crash\n");

    // Initialize RIST contexts (NULL logging for contexts)
    if (rist_receiver_create(&g_client.receiver_ctx, RIST_PROFILE_MAIN, NULL) != 0) {
        printf("Failed to create receiver context\n");
        return 1;
    }
    
    if (rist_sender_create(&g_client.sender_ctx, RIST_PROFILE_MAIN, 0, NULL) != 0) {
        printf("Failed to create sender context\n");
        return 1;
    }
    
    // Configure receiver (connect to server port 1706)
    struct rist_peer_config *receiver_config;
    if (rist_parse_address2("rist://192.168.10.139:1706", &receiver_config) != 0) {
        printf("Failed to parse receiver address\n");
        return 1;
    }
    
    // Apply optimized parameters identical to Snapcast
    struct rist_peer_config *mutable_receiver_config = receiver_config;
    mutable_receiver_config->recovery_length_min = 200;  // bufmin: 200ms minimum buffer for connection stability
    mutable_receiver_config->recovery_length_max = 200;  // bufmax: 200ms maximum buffer (still much lower than 1000ms default)
    mutable_receiver_config->recovery_rtt_min = 5;       // rttmin: 5ms (default)
    mutable_receiver_config->recovery_rtt_max = 500;     // rttmax: 500ms (default)
    mutable_receiver_config->recovery_reorder_buffer = 15;  // reorder: 15 packets (default)
    mutable_receiver_config->min_retries = 6;            // min_retries: 6 (default)
    mutable_receiver_config->max_retries = 20;           // max_retries: 20 (default)
    mutable_receiver_config->congestion_control_mode = 0;   // Default congestion control
    
    struct rist_peer *receiver_peer;
    if (rist_peer_create(g_client.receiver_ctx, &receiver_peer, receiver_config) != 0) {
        printf("Failed to create receiver peer\n");
        return 1;
    }
    
    // Configure sender (connect to server port 1708)
    struct rist_peer_config *sender_config;
    if (rist_parse_address2("rist://192.168.10.139:1708", &sender_config) != 0) {
        printf("Failed to parse sender address\n");
        return 1;
    }
    
    // Apply same parameters identical to Snapcast
    struct rist_peer_config *mutable_sender_config = sender_config;
    mutable_sender_config->recovery_length_min = 200;  // bufmin: 200ms minimum buffer for connection stability
    mutable_sender_config->recovery_length_max = 200;  // bufmax: 200ms maximum buffer (still much lower than 1000ms default)
    mutable_sender_config->recovery_rtt_min = 5;       // rttmin: 5ms (default)
    mutable_sender_config->recovery_rtt_max = 500;     // rttmax: 500ms (default)
    mutable_sender_config->recovery_reorder_buffer = 15;  // reorder: 15 packets (default)
    mutable_sender_config->min_retries = 6;            // min_retries: 6 (default)
    mutable_sender_config->max_retries = 20;           // max_retries: 20 (default)
    mutable_sender_config->congestion_control_mode = 0;   // Default congestion control
    
    struct rist_peer *sender_peer;
    if (rist_peer_create(g_client.sender_ctx, &sender_peer, sender_config) != 0) {
        printf("Failed to create sender peer\n");
        return 1;
    }
    
    // Set data callback for receiving from server
    if (rist_receiver_data_callback_set2(g_client.receiver_ctx, data_callback, NULL) != 0) {
        printf("Failed to set data callback\n");
        return 1;
    }
    
    // Set FIFO size like Snapcast
    if (rist_receiver_set_output_fifo_size(g_client.receiver_ctx, 64) != 0) {
        printf("Warning: Failed to set FIFO size\n");
    }
    
    // Start contexts
    if (rist_start(g_client.receiver_ctx) != 0) {
        printf("Failed to start receiver\n");
        return 1;
    }
    
    if (rist_start(g_client.sender_ctx) != 0) {
        printf("Failed to start sender\n");
        return 1;
    }
    
    print_timestamp();
    printf("[CLIENT] RIST client started, connecting to 192.168.10.139\n");
    printf("[CLIENT] Waiting for connections to establish...\n");
    
    // Wait for connections
    sleep(2);
    
    print_timestamp();
    printf("[CLIENT] Sending Hello message\n");
    send_hello();
    
    g_client.running = 1;
    int time_counter = 0;
    
    while (g_client.running) {
        usleep(100000); // 100ms
        
        // Send time requests occasionally (like Snapcast does)
        if ((time_counter++ % 10) == 0) {
            echo_last_number();
        }
        
        // Stop after 30 seconds for testing
        static int loop_count = 0;
        if (++loop_count > 300) { // 30 seconds at 100ms intervals
            print_timestamp();
            printf("[CLIENT] Test complete - shutting down\n");
            break;
        }
    }
    
    // Cleanup
    rist_destroy(g_client.receiver_ctx);
    rist_destroy(g_client.sender_ctx);
    
    return 0;
}