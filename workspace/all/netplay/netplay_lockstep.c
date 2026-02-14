/*
 * NextUI Lockstep Netplay Module
 * Pure lockstep synchronization: both devices must have same inputs before advancing.
 *
 * Key design:
 * - Frame buffer: circular buffer storing input history
 * - Host = Player 1, Client = Player 2 (always)
 * - Both devices run identical emulation with identical inputs
 * - Stalls if remote input hasn't arrived (never speculates)
 */

#define _GNU_SOURCE  // For strcasestr

#include "netplay_lockstep.h"
#include "netplay_helper.h"  // For stopHotspotAndRestoreWiFiAsync, netplay_connected_to_hotspot
#include "network_common.h"
#include "ra_protocol.h"     // For RA LAN discovery
#include "defines.h"  // Must come before api.h for BTN_ID_COUNT
#include "api.h"
#ifdef HAS_WIFIMG
#include "wifi_direct.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

// Protocol constants (internal)
#define NP_PROTOCOL_MAGIC   0x4E585550  // "NXUP" - NextUI Protocol
#define NP_DISCOVERY_QUERY  0x4E584451  // "NXDQ" - NextUI Discovery Query
#define NP_DISCOVERY_RESP   0x4E584452  // "NXDR" - NextUI Discovery Response

// Optimization: Discovery broadcast interval (microseconds)
#define DISCOVERY_BROADCAST_INTERVAL_US 500000  // 500ms

// Network commands
enum {
    CMD_INPUT      = 0x01,  // Input data for a frame
    CMD_STATE_REQ  = 0x02,  // Request state transfer
    CMD_STATE_HDR  = 0x03,  // State header (size)
    CMD_STATE_DATA = 0x04,  // State data chunk
    CMD_STATE_ACK  = 0x05,  // State received OK
    CMD_PING       = 0x06,
    CMD_PONG       = 0x07,
    CMD_DISCONNECT = 0x08,
    CMD_READY      = 0x09,  // Ready to play
    CMD_PAUSE      = 0x0A,  // Player paused (menu opened)
    CMD_RESUME     = 0x0B,  // Player resumed (menu closed)
    CMD_KEEPALIVE  = 0x0C,  // Keepalive during stall to prevent timeout
};

// Frame input entry
typedef struct {
    uint32_t frame;
    uint16_t p1_input;  // Host input (always Player 1)
    uint16_t p2_input;  // Client input (always Player 2)
    bool have_p1;
    bool have_p2;
} FrameInput;

// Packet header
typedef struct __attribute__((packed)) {
    uint8_t  cmd;
    uint32_t frame;
    uint16_t size;
} PacketHeader;

// Input packet
typedef struct __attribute__((packed)) {
    uint16_t input;
} InputPacket;

// Main lockstep state
static struct {
    NetplayMode mode;
    NetplayState state;

    // Sockets
    int tcp_fd;         // Main TCP connection
    int listen_fd;      // Server listen socket
    int udp_fd;         // Discovery UDP socket

    // Connection info
    char local_ip[16];
    char remote_ip[16];
    uint16_t port;

    // Game info
    char game_name[NETPLAY_MAX_GAME_NAME];
    uint32_t game_crc;

    // Frame synchronization
    uint32_t self_frame;        // Our current frame
    uint32_t run_frame;         // Frame we're executing
    uint32_t other_frame;       // Last frame with complete input

    // Circular frame buffer
    FrameInput frame_buffer[NETPLAY_FRAME_BUFFER_SIZE];

    // Local input for current frame
    uint16_t local_input;

    // State sync flags
    bool needs_state_sync;
    bool state_sync_complete;

    // Discovery
    NetplayHostInfo discovered_hosts[NETPLAY_MAX_HOSTS];
    int num_hosts;
    bool discovery_active;

    // RA LAN discovery (separate socket since RA uses port 55435, not 55436)
    int ra_discovery_fd;
    RA_DiscoveredHost ra_hosts[NETPLAY_MAX_HOSTS];
    int ra_num_hosts;
    struct timeval ra_last_query;

    // Threading
    pthread_t listen_thread;
    pthread_mutex_t mutex;
    volatile bool running;

    // Status
    char status_msg[128];
    int stall_frames;

    // Optimization: Cached audio silence state (updated per frame)
    volatile bool audio_should_silence;

    // Hotspot mode
    bool using_hotspot;

    // Pause state (for menu)
    bool local_paused;   // We have paused (menu open)
    bool remote_paused;  // Remote player has paused

    // Initialization flag
    bool initialized;

} ls = {0};

// Forward declarations
static bool send_packet(uint8_t cmd, uint32_t frame, const void* data, uint16_t size);
static bool recv_packet(PacketHeader* hdr, void* data, uint16_t max_size, int timeout_ms);
static void* listen_thread_func(void* arg);
static FrameInput* get_frame_slot(uint32_t frame);
static void init_frame_buffer(void);

//////////////////////////////////////////////////////////////////////////////
// Initialization
//////////////////////////////////////////////////////////////////////////////

void Lockstep_init(void) {
    if (ls.initialized) return;

    memset(&ls, 0, sizeof(ls));
    ls.mode = NETPLAY_OFF;
    ls.state = NETPLAY_STATE_IDLE;
    ls.tcp_fd = -1;
    ls.listen_fd = -1;
    ls.udp_fd = -1;
    ls.ra_discovery_fd = -1;
    ls.port = NETPLAY_DEFAULT_PORT;
    pthread_mutex_init(&ls.mutex, NULL);
    NET_getLocalIP(ls.local_ip, sizeof(ls.local_ip));
    snprintf(ls.status_msg, sizeof(ls.status_msg), "Netplay ready");
    ls.initialized = true;
}

void Lockstep_quit(void) {
    if (!ls.initialized) return;

    // Capture hotspot state before cleanup
    bool was_host = (ls.mode == NETPLAY_HOST);
    bool needs_hotspot_cleanup = ls.using_hotspot || netplay_connected_to_hotspot;

    Lockstep_disconnect();
    Lockstep_stopHostFast();
    Lockstep_stopDiscovery();

    // Handle hotspot cleanup asynchronously
    if (needs_hotspot_cleanup) {
        stopHotspotAndRestoreWiFiAsync(was_host);
        netplay_connected_to_hotspot = 0;
    }

    pthread_mutex_destroy(&ls.mutex);
    ls.initialized = false;
}

bool Lockstep_checkCoreSupport(const char* core_name) {
    if (strcasecmp(core_name, "fbneo") == 0 ||
        strcasecmp(core_name, "fceumm") == 0 ||
        strcasecmp(core_name, "snes9x") == 0 ||
        strcasecmp(core_name, "mednafen_supafaust") == 0 ||
        strcasecmp(core_name, "picodrive") == 0 ||
        strcasecmp(core_name, "pcsx_rearmed") == 0) {
        return true;
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////////
// Helper Functions
//////////////////////////////////////////////////////////////////////////////

static FrameInput* get_frame_slot(uint32_t frame) {
    return &ls.frame_buffer[frame & NETPLAY_FRAME_MASK];
}

static void init_frame_slot(uint32_t frame) {
    FrameInput* slot = get_frame_slot(frame);
    slot->frame = frame;
    slot->p1_input = 0;
    slot->p2_input = 0;
    slot->have_p1 = false;
    slot->have_p2 = false;
}

static void init_frame_buffer(void) {
    for (int i = 0; i < NETPLAY_FRAME_BUFFER_SIZE; i++) {
        init_frame_slot(i);
    }
}

//////////////////////////////////////////////////////////////////////////////
// Host Mode
//////////////////////////////////////////////////////////////////////////////

int Lockstep_startHost(const char* game_name, uint32_t game_crc, const char* hotspot_ip) {
    Lockstep_init();  // Lazy init
    if (ls.mode != NETPLAY_OFF) {
        return -1;
    }

    if (hotspot_ip) {
        ls.using_hotspot = true;
        strncpy(ls.local_ip, hotspot_ip, sizeof(ls.local_ip) - 1);
        ls.local_ip[sizeof(ls.local_ip) - 1] = '\0';
    }

    ls.listen_fd = NET_createListenSocket(ls.port, ls.status_msg, sizeof(ls.status_msg));
    if (ls.listen_fd < 0) {
        if (hotspot_ip) ls.using_hotspot = false;
        return -1;
    }

    ls.udp_fd = NET_createBroadcastSocket();
    if (ls.udp_fd < 0) {
        close(ls.listen_fd);
        ls.listen_fd = -1;
        if (hotspot_ip) ls.using_hotspot = false;
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Failed to create broadcast socket");
        return -1;
    }

    strncpy(ls.game_name, game_name, NETPLAY_MAX_GAME_NAME - 1);
    ls.game_crc = game_crc;

    ls.running = true;
    pthread_create(&ls.listen_thread, NULL, listen_thread_func, NULL);

    ls.mode = NETPLAY_HOST;
    ls.state = NETPLAY_STATE_WAITING;
    ls.needs_state_sync = true;

    snprintf(ls.status_msg, sizeof(ls.status_msg), "Hosting on %s:%d", ls.local_ip, ls.port);
    return 0;
}

void Lockstep_stopBroadcast(void) {
    if (ls.udp_fd >= 0) {
        close(ls.udp_fd);
        ls.udp_fd = -1;
    }
}

static void Lockstep_restartBroadcast(void) {
    if (ls.udp_fd >= 0) return;
    if (ls.mode != NETPLAY_HOST) return;

    ls.udp_fd = NET_createBroadcastSocket();
    if (ls.udp_fd < 0) {
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Failed to restart broadcast");
    }
}

static int Lockstep_stopHostInternal(bool skip_hotspot_cleanup) {
    if (ls.mode != NETPLAY_HOST) return -1;

    ls.running = false;

    if (ls.listen_fd >= 0) {
        shutdown(ls.listen_fd, SHUT_RDWR);
    }

    if (ls.listen_thread) {
        pthread_join(ls.listen_thread, NULL);
        ls.listen_thread = 0;
    }

    if (ls.listen_fd >= 0) {
        close(ls.listen_fd);
        ls.listen_fd = -1;
    }

    Lockstep_stopBroadcast();
    Lockstep_disconnect();

    if (ls.using_hotspot) {
        if (!skip_hotspot_cleanup) {
#ifdef HAS_WIFIMG
            WIFI_direct_stopHotspot();
#endif
        }
        ls.using_hotspot = false;
    }

    ls.mode = NETPLAY_OFF;
    ls.state = NETPLAY_STATE_IDLE;
    snprintf(ls.status_msg, sizeof(ls.status_msg), "Netplay ready");
    return 0;
}

int Lockstep_stopHost(void) {
    return Lockstep_stopHostInternal(false);
}

int Lockstep_stopHostFast(void) {
    return Lockstep_stopHostInternal(true);
}

static void* listen_thread_func(void* arg) {
    (void)arg;

    NET_BroadcastTimer broadcast_timer;
    NET_initBroadcastTimer(&broadcast_timer, DISCOVERY_BROADCAST_INTERVAL_US);

    while (ls.running && ls.listen_fd >= 0) {
        pthread_mutex_lock(&ls.mutex);
        bool is_waiting = (ls.state == NETPLAY_STATE_WAITING);
        int udp_fd = ls.udp_fd;
        pthread_mutex_unlock(&ls.mutex);

        if (udp_fd >= 0 && is_waiting) {
            if (NET_shouldBroadcast(&broadcast_timer)) {
                NET_sendDiscoveryBroadcast(udp_fd, NP_DISCOVERY_RESP, NETPLAY_PROTOCOL_VERSION,
                                           ls.game_crc, ls.port, NETPLAY_DISCOVERY_PORT,
                                           ls.game_name, NULL);
            }
        }

        if (is_waiting) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ls.listen_fd, &fds);

            struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
            if (select(ls.listen_fd + 1, &fds, NULL, NULL, &tv) > 0) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);

                int fd = accept(ls.listen_fd, (struct sockaddr*)&client_addr, &len);
                if (fd >= 0) {
                    pthread_mutex_lock(&ls.mutex);

                    if (ls.state != NETPLAY_STATE_WAITING) {
                        close(fd);
                        pthread_mutex_unlock(&ls.mutex);
                        continue;
                    }

                    NET_configureTCPSocket(fd, NULL);

                    ls.tcp_fd = fd;
                    inet_ntop(AF_INET, &client_addr.sin_addr, ls.remote_ip, sizeof(ls.remote_ip));

                    ls.state = NETPLAY_STATE_SYNCING;
                    ls.needs_state_sync = true;
                    ls.self_frame = 0;
                    ls.run_frame = 0;
                    ls.other_frame = 0;

                    init_frame_buffer();

                    snprintf(ls.status_msg, sizeof(ls.status_msg), "Client connected: %s", ls.remote_ip);
                    pthread_mutex_unlock(&ls.mutex);
                }
            }
        } else {
            usleep(50000);
        }
    }

    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// Client Mode
//////////////////////////////////////////////////////////////////////////////

int Lockstep_connectToHost(const char* ip, uint16_t port) {
    Lockstep_init();  // Lazy init
    if (ls.mode != NETPLAY_OFF) {
        return -1;
    }

    ls.tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ls.tcp_fd < 0) {
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Socket creation failed");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(ls.tcp_fd);
        ls.tcp_fd = -1;
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Invalid IP address");
        return -1;
    }

    ls.state = NETPLAY_STATE_CONNECTING;
    snprintf(ls.status_msg, sizeof(ls.status_msg), "Connecting to %s:%d...", ip, port);

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(ls.tcp_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(ls.tcp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(ls.tcp_fd);
        ls.tcp_fd = -1;
        ls.state = NETPLAY_STATE_ERROR;
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Connection failed");
        return -1;
    }

    NET_configureTCPSocket(ls.tcp_fd, NULL);

    strncpy(ls.remote_ip, ip, sizeof(ls.remote_ip) - 1);
    ls.port = port;
    ls.mode = NETPLAY_CLIENT;
    ls.state = NETPLAY_STATE_SYNCING;
    ls.needs_state_sync = true;

    ls.self_frame = 0;
    ls.run_frame = 0;
    ls.other_frame = 0;

    init_frame_buffer();

    snprintf(ls.status_msg, sizeof(ls.status_msg), "Connected to %s", ip);
    return 0;
}

void Lockstep_disconnect(void) {
    if (ls.tcp_fd >= 0) {
        send_packet(CMD_DISCONNECT, 0, NULL, 0);
        close(ls.tcp_fd);
        ls.tcp_fd = -1;
    }

    ls.audio_should_silence = false;
    ls.local_paused = false;
    ls.remote_paused = false;

    if (ls.mode == NETPLAY_CLIENT) {
        ls.mode = NETPLAY_OFF;
        ls.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Disconnected");
    } else if (ls.mode == NETPLAY_HOST) {
        ls.state = NETPLAY_STATE_WAITING;
        ls.needs_state_sync = true;
        ls.stall_frames = 0;
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Client left, waiting on %s:%d", ls.local_ip, ls.port);
    } else {
        ls.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Disconnected");
    }
}

//////////////////////////////////////////////////////////////////////////////
// Discovery
//////////////////////////////////////////////////////////////////////////////

int Lockstep_startDiscovery(void) {
    if (ls.discovery_active) return 0;

    ls.udp_fd = NET_createDiscoveryListenSocket(NETPLAY_DISCOVERY_PORT);
    if (ls.udp_fd < 0) {
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Failed to start discovery");
        return -1;
    }

    // Also create a socket for RA LAN discovery (port 55435)
    // RA hosts listen on their netplay port for UDP discovery queries
    ls.ra_discovery_fd = NET_createDiscoveryListenSocket(RA_DISCOVERY_PORT);
    if (ls.ra_discovery_fd >= 0) {
        // Enable broadcast so we can send queries
        int broadcast = 1;
        setsockopt(ls.ra_discovery_fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        // Send initial RA discovery query
        RA_sendDiscoveryQuery(ls.ra_discovery_fd);
        gettimeofday(&ls.ra_last_query, NULL);
    }

    ls.num_hosts = 0;
    ls.ra_num_hosts = 0;
    ls.discovery_active = true;
    return 0;
}

void Lockstep_stopDiscovery(void) {
    if (!ls.discovery_active) return;

    if (ls.udp_fd >= 0 && ls.mode == NETPLAY_OFF) {
        close(ls.udp_fd);
        ls.udp_fd = -1;
    }

    if (ls.ra_discovery_fd >= 0) {
        close(ls.ra_discovery_fd);
        ls.ra_discovery_fd = -1;
    }

    ls.ra_num_hosts = 0;
    ls.discovery_active = false;
}

int Lockstep_getDiscoveredHosts(NetplayHostInfo* hosts, int max_hosts) {
    if (!ls.discovery_active || ls.udp_fd < 0) return 0;

    // Poll for NextUI host broadcasts
    NET_receiveDiscoveryResponses(ls.udp_fd, NP_DISCOVERY_RESP,
                                   (NET_HostInfo*)ls.discovered_hosts, &ls.num_hosts,
                                   NETPLAY_MAX_HOSTS);

    // Poll for RA host responses and re-send queries periodically
    if (ls.ra_discovery_fd >= 0) {
        RA_receiveDiscoveryResponses(ls.ra_discovery_fd, ls.ra_hosts,
                                      &ls.ra_num_hosts, NETPLAY_MAX_HOSTS);

        // Re-send RA discovery query every 500ms (same as NextUI broadcast interval)
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_us = (now.tv_sec - ls.ra_last_query.tv_sec) * 1000000 +
                          (now.tv_usec - ls.ra_last_query.tv_usec);
        if (elapsed_us >= DISCOVERY_BROADCAST_INTERVAL_US) {
            RA_sendDiscoveryQuery(ls.ra_discovery_fd);
            ls.ra_last_query = now;
        }
    }

    // Merge results: NextUI hosts first, then RA hosts
    int count = 0;

    // Copy NextUI hosts
    for (int i = 0; i < ls.num_hosts && count < max_hosts; i++) {
        hosts[count++] = ls.discovered_hosts[i];
    }

    // Merge RA hosts (skip duplicates by IP)
    for (int i = 0; i < ls.ra_num_hosts && count < max_hosts; i++) {
        bool duplicate = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(hosts[j].host_ip, ls.ra_hosts[i].host_ip) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            NetplayHostInfo* h = &hosts[count];
            // Use RA content name (or nick if content is empty) as game_name
            if (ls.ra_hosts[i].content[0]) {
                strncpy(h->game_name, ls.ra_hosts[i].content, NETPLAY_MAX_GAME_NAME - 1);
            } else {
                snprintf(h->game_name, NETPLAY_MAX_GAME_NAME, "RA: %s", ls.ra_hosts[i].nick);
            }
            h->game_name[NETPLAY_MAX_GAME_NAME - 1] = '\0';
            strncpy(h->host_ip, ls.ra_hosts[i].host_ip, sizeof(h->host_ip) - 1);
            h->host_ip[sizeof(h->host_ip) - 1] = '\0';
            h->port = ls.ra_hosts[i].port;
            h->game_crc = ls.ra_hosts[i].content_crc;
            count++;
        }
    }

    return count;
}

//////////////////////////////////////////////////////////////////////////////
// Frame Synchronization
//////////////////////////////////////////////////////////////////////////////

bool Lockstep_preFrame(void) {
    pthread_mutex_lock(&ls.mutex);

    if (ls.tcp_fd < 0 ||
        (ls.state != NETPLAY_STATE_SYNCING &&
         ls.state != NETPLAY_STATE_PLAYING &&
         ls.state != NETPLAY_STATE_STALLED &&
         ls.state != NETPLAY_STATE_PAUSED)) {
        pthread_mutex_unlock(&ls.mutex);
        return true;
    }

    FrameInput* run_slot = get_frame_slot(ls.run_frame);

    FrameInput* input_slot = get_frame_slot(ls.self_frame);
    if (input_slot->frame != ls.self_frame) {
        init_frame_slot(ls.self_frame);
        input_slot->frame = ls.self_frame;
    }

    if (ls.mode == NETPLAY_HOST) {
        if (!input_slot->have_p1) {
            input_slot->p1_input = ls.local_input;
            input_slot->have_p1 = true;
            InputPacket pkt = { .input = htons(ls.local_input) };
            send_packet(CMD_INPUT, ls.self_frame, &pkt, sizeof(pkt));
        }
    } else {
        if (!input_slot->have_p2) {
            input_slot->p2_input = ls.local_input;
            input_slot->have_p2 = true;
            InputPacket pkt = { .input = htons(ls.local_input) };
            send_packet(CMD_INPUT, ls.self_frame, &pkt, sizeof(pkt));
        }
    }

    int timeout_ms = 16;
    int max_attempts = 10;
    int attempts = 0;

    while (attempts < max_attempts) {
        run_slot = get_frame_slot(ls.run_frame);
        if (run_slot->have_p1 && run_slot->have_p2) {
            break;
        }

        pthread_mutex_unlock(&ls.mutex);

        PacketHeader hdr;
        InputPacket remote_pkt;
        bool received = recv_packet(&hdr, &remote_pkt, sizeof(remote_pkt), timeout_ms);

        pthread_mutex_lock(&ls.mutex);

        if (ls.state == NETPLAY_STATE_DISCONNECTED) {
            ls.audio_should_silence = false;
            pthread_mutex_unlock(&ls.mutex);
            return false;
        }

        if (received) {
            if (hdr.cmd == CMD_INPUT) {
                FrameInput* remote_slot = get_frame_slot(hdr.frame);
                uint16_t remote_input = ntohs(remote_pkt.input);

                if (ls.mode == NETPLAY_HOST) {
                    remote_slot->p2_input = remote_input;
                    remote_slot->have_p2 = true;
                } else {
                    remote_slot->p1_input = remote_input;
                    remote_slot->have_p1 = true;
                }
            } else if (hdr.cmd == CMD_DISCONNECT) {
                close(ls.tcp_fd);
                ls.tcp_fd = -1;
                ls.audio_should_silence = false;

                if (ls.mode == NETPLAY_HOST) {
                    ls.state = NETPLAY_STATE_WAITING;
                    ls.needs_state_sync = true;
                    ls.stall_frames = 0;
                    Lockstep_restartBroadcast();
                    snprintf(ls.status_msg, sizeof(ls.status_msg), "Client left, waiting on %s:%d", ls.local_ip, ls.port);
                } else {
                    ls.state = NETPLAY_STATE_DISCONNECTED;
                    snprintf(ls.status_msg, sizeof(ls.status_msg), "Host disconnected");
                }
                pthread_mutex_unlock(&ls.mutex);
                return false;
            } else if (hdr.cmd == CMD_PAUSE) {
                ls.remote_paused = true;
                ls.state = NETPLAY_STATE_PAUSED;
                snprintf(ls.status_msg, sizeof(ls.status_msg), "Remote player paused");
            } else if (hdr.cmd == CMD_RESUME) {
                ls.remote_paused = false;
                if (!ls.local_paused) {
                    ls.state = NETPLAY_STATE_PLAYING;
                    snprintf(ls.status_msg, sizeof(ls.status_msg), "Netplay active");
                }
            } else if (hdr.cmd == CMD_KEEPALIVE) {
                // Keepalive received - reset stall counter
            }
        }
        attempts++;
    }

    run_slot = get_frame_slot(ls.run_frame);
    if (!run_slot->have_p1 || !run_slot->have_p2) {
        ls.stall_frames++;

        if (ls.stall_frames % NETPLAY_KEEPALIVE_INTERVAL_FRAMES == 0) {
            send_packet(CMD_KEEPALIVE, ls.self_frame, NULL, 0);
        }

        if (!ls.local_paused && !ls.remote_paused) {
            if (ls.stall_frames > NETPLAY_STALL_TIMEOUT_FRAMES) {
                snprintf(ls.status_msg, sizeof(ls.status_msg), "Connection timeout");
                ls.state = NETPLAY_STATE_DISCONNECTED;
                ls.audio_should_silence = false;
                pthread_mutex_unlock(&ls.mutex);
                return false;
            } else if (ls.stall_frames > NETPLAY_STALL_WARNING_FRAMES) {
                int remaining = (NETPLAY_STALL_TIMEOUT_FRAMES - ls.stall_frames) / 60;
                snprintf(ls.status_msg, sizeof(ls.status_msg), "Waiting... (%ds)", remaining);
            }
        }
        ls.state = NETPLAY_STATE_STALLED;
        ls.audio_should_silence = true;
        pthread_mutex_unlock(&ls.mutex);
        return false;
    }

    ls.stall_frames = 0;
    ls.audio_should_silence = false;
    ls.state = NETPLAY_STATE_PLAYING;
    pthread_mutex_unlock(&ls.mutex);
    return true;
}

uint16_t Lockstep_getInputState(unsigned port) {
    if (!Lockstep_isConnected()) return 0;

    pthread_mutex_lock(&ls.mutex);
    FrameInput* slot = get_frame_slot(ls.run_frame);
    uint16_t input = (port == 0) ? slot->p1_input : slot->p2_input;
    pthread_mutex_unlock(&ls.mutex);

    return input;
}

uint32_t Lockstep_getPlayerButtons(unsigned port, uint32_t local_buttons) {
    if (ls.mode != NETPLAY_OFF && Lockstep_isConnected()) {
        return Lockstep_getInputState(port);
    }
    return (port == 0) ? local_buttons : 0;
}

void Lockstep_setLocalInput(uint16_t input) {
    ls.local_input = input;
}

void Lockstep_postFrame(void) {
    if (!Lockstep_isConnected()) return;

    pthread_mutex_lock(&ls.mutex);
    ls.run_frame++;
    ls.self_frame++;
    pthread_mutex_unlock(&ls.mutex);
}

bool Lockstep_shouldStall(void) {
    return ls.state == NETPLAY_STATE_STALLED;
}

bool Lockstep_shouldSilenceAudio(void) {
    return ls.audio_should_silence;
}

//////////////////////////////////////////////////////////////////////////////
// State Synchronization
//////////////////////////////////////////////////////////////////////////////

int Lockstep_sendState(const void* data, size_t size) {
    if (!Lockstep_isConnected() || !data || size == 0) return -1;

    uint32_t state_size = (uint32_t)size;
    state_size = htonl(state_size);
    if (!send_packet(CMD_STATE_HDR, 0, &state_size, sizeof(state_size))) {
        return -1;
    }

    const uint8_t* ptr = (const uint8_t*)data;
    size_t remaining = size;

    while (remaining > 0) {
        size_t chunk = (remaining > 4096) ? 4096 : remaining;
        ssize_t sent = send(ls.tcp_fd, ptr, chunk, MSG_NOSIGNAL);
        if (sent <= 0) {
            return -1;
        }
        ptr += sent;
        remaining -= sent;
    }

    PacketHeader hdr;
    if (!recv_packet(&hdr, NULL, 0, 10000) || hdr.cmd != CMD_STATE_ACK) {
        return -1;
    }

    if (!send_packet(CMD_READY, 0, NULL, 0)) {
        return -1;
    }

    return 0;
}

int Lockstep_receiveState(void* data, size_t size) {
    if (!Lockstep_isConnected() || !data || size == 0) return -1;

    PacketHeader hdr;
    uint32_t state_size;

    if (!recv_packet(&hdr, &state_size, sizeof(state_size), 10000) ||
        hdr.cmd != CMD_STATE_HDR) {
        return -1;
    }

    state_size = ntohl(state_size);

    if (state_size != size) {
        snprintf(ls.status_msg, sizeof(ls.status_msg),
                 "State size mismatch: %u vs %zu", state_size, size);
        return -1;
    }

    uint8_t* ptr = (uint8_t*)data;
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t received = recv(ls.tcp_fd, ptr, remaining, 0);
        if (received <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        ptr += received;
        remaining -= received;
    }

    if (!send_packet(CMD_STATE_ACK, 0, NULL, 0)) {
        return -1;
    }

    if (!recv_packet(&hdr, NULL, 0, 10000) || hdr.cmd != CMD_READY) {
        return -1;
    }

    return 0;
}

bool Lockstep_needsStateSync(void) {
    return ls.needs_state_sync && ls.state == NETPLAY_STATE_SYNCING;
}

void Lockstep_completeStateSync(void) {
    pthread_mutex_lock(&ls.mutex);
    ls.needs_state_sync = false;
    ls.state_sync_complete = true;
    ls.state = NETPLAY_STATE_PLAYING;

    for (int i = 0; i < NETPLAY_INPUT_LATENCY_FRAMES; i++) {
        FrameInput* slot = get_frame_slot(i);
        slot->frame = i;
        slot->p1_input = 0;
        slot->p2_input = 0;
        slot->have_p1 = true;
        slot->have_p2 = true;
    }

    ls.run_frame = 0;
    ls.self_frame = NETPLAY_INPUT_LATENCY_FRAMES;
    ls.stall_frames = 0;
    ls.audio_should_silence = false;

    snprintf(ls.status_msg, sizeof(ls.status_msg), "Netplay active");
    pthread_mutex_unlock(&ls.mutex);
}

//////////////////////////////////////////////////////////////////////////////
// Status Functions
//////////////////////////////////////////////////////////////////////////////

NetplayMode  Lockstep_getMode(void) { return ls.mode; }
NetplayState Lockstep_getState(void) { return ls.state; }
bool Lockstep_isUsingHotspot(void) { return ls.using_hotspot; }

bool Lockstep_isConnected(void) {
    return ls.tcp_fd >= 0 &&
           (ls.state == NETPLAY_STATE_SYNCING ||
            ls.state == NETPLAY_STATE_PLAYING ||
            ls.state == NETPLAY_STATE_STALLED ||
            ls.state == NETPLAY_STATE_PAUSED);
}

bool Lockstep_isActive(void) {
    return ls.state == NETPLAY_STATE_PLAYING;
}

const char* Lockstep_getStatusMessage(void) { return ls.status_msg; }

const char* Lockstep_getLocalIP(void) {
    if (ls.mode == NETPLAY_OFF) {
        NET_getLocalIP(ls.local_ip, sizeof(ls.local_ip));
    }
    return ls.local_ip;
}

bool Lockstep_hasNetworkConnection(void) {
    NET_getLocalIP(ls.local_ip, sizeof(ls.local_ip));
    return NET_hasConnection();
}

int Lockstep_getTcpFd(void) {
    return ls.tcp_fd;
}

int Lockstep_detachTcpFd(void) {
    int fd = ls.tcp_fd;
    ls.tcp_fd = -1;
    ls.mode = NETPLAY_OFF;
    ls.state = NETPLAY_STATE_IDLE;
    ls.needs_state_sync = false;
    return fd;
}

//////////////////////////////////////////////////////////////////////////////
// Pause/Resume for Menu
//////////////////////////////////////////////////////////////////////////////

void Lockstep_pause(void) {
    if (!Lockstep_isConnected()) return;

    pthread_mutex_lock(&ls.mutex);
    ls.local_paused = true;
    send_packet(CMD_PAUSE, 0, NULL, 0);
    ls.state = NETPLAY_STATE_PAUSED;
    snprintf(ls.status_msg, sizeof(ls.status_msg), "Paused");
    pthread_mutex_unlock(&ls.mutex);
}

void Lockstep_resume(void) {
    if (!Lockstep_isConnected()) return;

    pthread_mutex_lock(&ls.mutex);
    ls.local_paused = false;
    send_packet(CMD_RESUME, 0, NULL, 0);

    if (!ls.remote_paused) {
        ls.state = NETPLAY_STATE_PLAYING;
        ls.stall_frames = 0;
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Netplay active");
    } else {
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Waiting for remote...");
    }
    pthread_mutex_unlock(&ls.mutex);
}

void Lockstep_pollWhilePaused(void) {
    if (!Lockstep_isConnected()) return;

    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(ls.tcp_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        pthread_mutex_lock(&ls.mutex);
        ls.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Connection lost");
        close(ls.tcp_fd);
        ls.tcp_fd = -1;
        pthread_mutex_unlock(&ls.mutex);
    }
}

bool Lockstep_isPaused(void) {
    return ls.local_paused || ls.remote_paused;
}

//////////////////////////////////////////////////////////////////////////////
// Main Loop Update (lockstep only, no protocol detection)
//////////////////////////////////////////////////////////////////////////////

int Lockstep_update(uint16_t local_input,
                    Netplay_SerializeSizeFn serialize_size_fn,
                    Netplay_SerializeFn serialize_fn,
                    Netplay_UnserializeFn unserialize_fn) {
    // Handle state sync when connection is established
    if (Lockstep_needsStateSync()) {
        if (!serialize_size_fn || !serialize_fn || !unserialize_fn) {
            Lockstep_disconnect();
            return 1;
        }

        size_t state_size = serialize_size_fn();
        bool sync_success = false;

        if (state_size > 0) {
            void* state_data = malloc(state_size);
            if (state_data) {
                if (ls.mode == NETPLAY_HOST) {
                    if (serialize_fn(state_data, state_size)) {
                        if (Lockstep_sendState(state_data, state_size) == 0) {
                            sync_success = true;
                        }
                    }
                } else {
                    if (Lockstep_receiveState(state_data, state_size) == 0) {
                        if (unserialize_fn(state_data, state_size)) {
                            sync_success = true;
                        }
                    }
                }
                free(state_data);
            }
        }

        if (sync_success) {
            Lockstep_completeStateSync();
        } else {
            Lockstep_disconnect();
        }
        return 0;
    }

    // Frame synchronization
    if (Lockstep_isActive() || Lockstep_shouldStall()) {
        Lockstep_setLocalInput(local_input);

        if (!Lockstep_preFrame()) {
            if (ls.state == NETPLAY_STATE_DISCONNECTED) {
                Lockstep_disconnect();
                return 1;
            }
            return 0;
        }
    }

    return 1;
}

//////////////////////////////////////////////////////////////////////////////
// Network Helper Functions
//////////////////////////////////////////////////////////////////////////////

static bool send_packet(uint8_t cmd, uint32_t frame, const void* data, uint16_t size) {
    if (ls.tcp_fd < 0) return false;

    PacketHeader hdr = {
        .cmd = cmd,
        .frame = htonl(frame),
        .size = htons(size)
    };

    if (send(ls.tcp_fd, &hdr, sizeof(hdr), MSG_NOSIGNAL) != sizeof(hdr)) {
        return false;
    }

    if (size > 0 && data) {
        if (send(ls.tcp_fd, data, size, MSG_NOSIGNAL) != size) {
            return false;
        }
    }

    return true;
}

static void handle_recv_disconnect(void) {
    pthread_mutex_lock(&ls.mutex);

    if (ls.tcp_fd >= 0) {
        close(ls.tcp_fd);
        ls.tcp_fd = -1;
    }

    if (ls.mode == NETPLAY_HOST) {
        ls.state = NETPLAY_STATE_WAITING;
        ls.needs_state_sync = true;
        ls.stall_frames = 0;
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Client left, waiting on %s:%d", ls.local_ip, ls.port);
        pthread_mutex_unlock(&ls.mutex);
        Lockstep_restartBroadcast();
    } else {
        ls.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(ls.status_msg, sizeof(ls.status_msg), "Remote disconnected");
        pthread_mutex_unlock(&ls.mutex);
    }
}

static bool recv_packet(PacketHeader* hdr, void* data, uint16_t max_size, int timeout_ms) {
    if (ls.tcp_fd < 0) return false;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(ls.tcp_fd, &fds);

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };

    if (select(ls.tcp_fd + 1, &fds, NULL, NULL, &tv) <= 0) {
        return false;
    }

    ssize_t ret = recv(ls.tcp_fd, hdr, sizeof(*hdr), 0);
    if (ret == 0) {
        handle_recv_disconnect();
        return false;
    }
    if (ret < 0 || ret != sizeof(*hdr)) {
        if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
            handle_recv_disconnect();
        }
        return false;
    }

    hdr->frame = ntohl(hdr->frame);
    hdr->size = ntohs(hdr->size);

    if (hdr->size > 4096) {
        return false;
    }

    if (hdr->size > 0 && data && hdr->size <= max_size) {
        ret = recv(ls.tcp_fd, data, hdr->size, 0);
        if (ret == 0) {
            handle_recv_disconnect();
            return false;
        }
        if (ret != hdr->size) {
            return false;
        }
    }

    return true;
}
