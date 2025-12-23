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
    int client_connected;
    uint32_t msg_counter;
} server_state_t;

static server_state_t g_server = {0};

// Message header structure (mimicking Snapcast)
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

void send_message(int vport, uint16_t msg_type, uint32_t data_size, const void* data) {
    msg_header_t header = {0};
    header.type = msg_type;
    header.id = ++g_server.msg_counter;
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
    
    int ret = rist_sender_data_write(g_server.sender_ctx, &data_block);
    print_timestamp();
    printf("[SERVER] Sent msg_type=%d, id=%d, size=%d bytes to vport %d (ret=%d)\n",
           msg_type, header.id, total_size, vport, ret);
    
    free(buffer);
}

void send_server_settings() {
    // Simulate ServerSettings with increasing number
    uint32_t settings_data = g_server.msg_counter + 1000;
    send_message(VPORT_CONTROL, MSG_SERVER_SETTINGS, sizeof(settings_data), &settings_data);
}

void send_codec_header() {
    // Simulate CodecHeader with larger data (like Snapcast's 1374 bytes)
    uint8_t codec_data[1374];
    for (int i = 0; i < sizeof(codec_data); i++) {
        codec_data[i] = (uint8_t)(i % 256);
    }
    send_message(VPORT_AUDIO, MSG_CODEC_HEADER, sizeof(codec_data), codec_data);
}

void send_time_response() {
    uint64_t time_data = g_server.msg_counter + 2000;
    send_message(VPORT_CONTROL, MSG_TIME, sizeof(time_data), &time_data);
}

void send_audio_chunk() {
    // Simulate 20ms audio chunk (like Snapcast's 3840 bytes)
    uint8_t audio_data[3840];
    srand(time(NULL) + g_server.msg_counter);
    for (int i = 0; i < sizeof(audio_data); i++) {
        audio_data[i] = (uint8_t)(rand() % 256);
    }
    send_message(VPORT_AUDIO, MSG_AUDIO_CHUNK, sizeof(audio_data), audio_data);
}

int data_callback(void *arg, struct rist_data_block *data_block) {
    print_timestamp();
    printf("[SERVER] Received %zu bytes on vport %d\n", data_block->payload_len, data_block->virt_dst_port);
    
    if (data_block->payload_len >= sizeof(msg_header_t)) {
        msg_header_t* header = (msg_header_t*)data_block->payload;
        print_timestamp();
        printf("[SERVER] Message: type=%d, id=%d, size=%d\n", header->type, header->id, header->size);
        
        switch (header->type) {
            case MSG_HELLO:
                print_timestamp();
                printf("[SERVER] Received Hello - sending ServerSettings and CodecHeader\n");
                g_server.client_connected = 1;
                send_server_settings();
                send_codec_header();
                break;
                
            case MSG_TIME:
                print_timestamp();
                printf("[SERVER] Received Time - sending Time response\n");
                send_time_response();
                break;
                
            default:
                print_timestamp();
                printf("[SERVER] Unknown message type: %d\n", header->type);
                break;
        }
    }
    
    return 0;
}

int main() {
    printf("=== RIST Test Server ===\n");
    
    // Initialize RIST contexts
    if (rist_sender_create(&g_server.sender_ctx, RIST_PROFILE_MAIN, 0, NULL) != 0) {
        printf("Failed to create sender context\n");
        return 1;
    }
    
    if (rist_receiver_create(&g_server.receiver_ctx, RIST_PROFILE_MAIN, NULL) != 0) {
        printf("Failed to create receiver context\n");
        return 1;
    }
    
    // Configure sender (bind to port 1706 like Snapcast)
    struct rist_peer_config *sender_config;
    if (rist_parse_address2("rist://@0.0.0.0:1706", &sender_config) != 0) {
        printf("Failed to parse sender address\n");
        return 1;
    }
    
    // Apply optimized parameters like Snapcast
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
    if (rist_peer_create(g_server.sender_ctx, &sender_peer, sender_config) != 0) {
        printf("Failed to create sender peer\n");
        return 1;
    }
    
    // Configure receiver (bind to port 1708 like Snapcast)
    struct rist_peer_config *receiver_config;
    if (rist_parse_address2("rist://@0.0.0.0:1708", &receiver_config) != 0) {
        printf("Failed to parse receiver address\n");
        return 1;
    }
    
    // Apply same parameters identical to Snapcast
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
    if (rist_peer_create(g_server.receiver_ctx, &receiver_peer, receiver_config) != 0) {
        printf("Failed to create receiver peer\n");
        return 1;
    }
    
    // Set data callback for receiving backchannel
    if (rist_receiver_data_callback_set2(g_server.receiver_ctx, data_callback, NULL) != 0) {
        printf("Failed to set data callback\n");
        return 1;
    }
    
    // Start contexts
    if (rist_start(g_server.sender_ctx) != 0) {
        printf("Failed to start sender\n");
        return 1;
    }
    
    if (rist_start(g_server.receiver_ctx) != 0) {
        printf("Failed to start receiver\n");
        return 1;
    }
    
    print_timestamp();
    printf("[SERVER] RIST server started on ports 1706 (sender) and 1708 (receiver)\n");
    printf("[SERVER] Waiting for client connection...\n");
    
    g_server.running = 1;
    int audio_counter = 0;
    
    while (g_server.running) {
        usleep(50000); // 50ms
        
        // Send audio chunks every 20ms once client is connected
        if (g_server.client_connected && (audio_counter++ % 1) == 0) {
            send_audio_chunk();
        }
        
        // Stop after 30 seconds for testing
        static int loop_count = 0;
        if (++loop_count > 600) { // 30 seconds at 50ms intervals
            print_timestamp();
            printf("[SERVER] Test complete - shutting down\n");
            break;
        }
    }
    
    // Cleanup
    rist_destroy(g_server.sender_ctx);
    rist_destroy(g_server.receiver_ctx);
    
    return 0;
}