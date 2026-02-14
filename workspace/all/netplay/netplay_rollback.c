/*
 * NextUI Rollback Netplay Engine
 * Implements speculative execution + rewind/replay for RA host compatibility.
 */

#include "netplay_rollback.h"
#include "ra_protocol.h"
#include "network_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#ifndef LOG_info
#define LOG_info(...) fprintf(stderr, __VA_ARGS__)
#endif

//////////////////////////////////////////////////////////////////////////////
// CRC32 (basic implementation for state verification)
//////////////////////////////////////////////////////////////////////////////

static uint32_t crc32_table[256];
static bool crc32_table_init = false;

static void init_crc32_table(void) {
    if (crc32_table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_init = true;
}

static uint32_t compute_crc32(const void* data, size_t size) {
    init_crc32_table();
    const uint8_t* buf = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

//////////////////////////////////////////////////////////////////////////////
// Singleton state
//////////////////////////////////////////////////////////////////////////////

static RollbackState rb = {0};

// Current frame being replayed (used by Rollback_getInput during replay)
static uint32_t replay_frame = 0;

//////////////////////////////////////////////////////////////////////////////
// Internal helpers
//////////////////////////////////////////////////////////////////////////////

static RollbackFrameSlot* get_slot(uint32_t frame) {
    return &rb.frames[frame & ROLLBACK_BUFFER_MASK];
}

static void init_slot(uint32_t frame) {
    RollbackFrameSlot* slot = get_slot(frame);
    slot->local_input = 0;
    slot->remote_input = 0;
    slot->remote_confirmed = false;
    slot->crc = 0;
    slot->state_saved = false;
}

// Save current core state into the ring buffer for `frame`
static bool save_state(uint32_t frame) {
    uint32_t idx = frame & ROLLBACK_BUFFER_MASK;
    if (!rb.state_buffer[idx]) return false;

    if (!rb.serialize_fn(rb.state_buffer[idx], rb.state_size)) {
        LOG_info("Rollback: failed to serialize state for frame %u\n", frame);
        return false;
    }
    get_slot(frame)->state_saved = true;
    return true;
}

// Load core state from the ring buffer for `frame`
static bool load_state(uint32_t frame) {
    uint32_t idx = frame & ROLLBACK_BUFFER_MASK;
    if (!rb.state_buffer[idx]) return false;

    RollbackFrameSlot* slot = get_slot(frame);
    if (!slot->state_saved) {
        LOG_info("Rollback: no saved state for frame %u\n", frame);
        return false;
    }

    if (!rb.unserialize_fn(rb.state_buffer[idx], rb.state_size)) {
        LOG_info("Rollback: failed to unserialize state for frame %u\n", frame);
        return false;
    }
    return true;
}

// Check if there's data available on the socket (non-blocking)
static bool has_pending_data(int fd) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {0, 0};  // Non-blocking
    return select(fd + 1, &fds, NULL, NULL, &tv) > 0;
}

// Receive and process all pending RA commands. Returns the oldest frame
// where a prediction was wrong, or UINT32_MAX if all predictions were correct.
static uint32_t process_incoming(void) {
    uint32_t oldest_wrong = UINT32_MAX;

    while (rb.tcp_fd >= 0 && has_pending_data(rb.tcp_fd)) {
        RA_PacketHeader hdr;
        uint8_t buf[4096];

        if (!RA_recvCmd(rb.tcp_fd, &hdr, buf, sizeof(buf), 0)) {
            break;
        }

        switch (hdr.cmd) {
        case RA_CMD_INPUT: {
            uint32_t frame_num, player_num;
            uint16_t input;
            if (!RA_parseInput(buf, hdr.size, &frame_num, &player_num, &input)) {
                LOG_info("Rollback: failed to parse CMD_INPUT\n");
                break;
            }

            // We only care about the host's input (player 0 / server input)
            // Our own input echoed back can be ignored
            if (player_num == rb.client_num) break;

            RollbackFrameSlot* slot = get_slot(frame_num);
            if (slot->remote_confirmed) break;  // Already have real input

            // Check if prediction was wrong
            if (frame_num < rb.self_frame && slot->remote_input != input) {
                if (frame_num < oldest_wrong) {
                    oldest_wrong = frame_num;
                }
            }

            slot->remote_input = input;
            slot->remote_confirmed = true;

            // Update read_frame (track latest confirmed)
            if (frame_num > rb.read_frame || rb.read_frame == 0) {
                rb.read_frame = frame_num;
            }
            break;
        }

        case RA_CMD_CRC: {
            // Server's CRC for a frame - we can compare with ours for desync detection
            if (hdr.size >= 8) {
                uint32_t frame_num = ntohl(*(uint32_t*)buf);
                uint32_t server_crc = ntohl(*(uint32_t*)(buf + 4));
                RollbackFrameSlot* slot = get_slot(frame_num);
                if (slot->crc != 0 && slot->crc != server_crc) {
                    LOG_info("Rollback: DESYNC at frame %u (local=0x%08x server=0x%08x)\n",
                             frame_num, slot->crc, server_crc);
                    rb.desync_detected = true;
                }
            }
            break;
        }

        case RA_CMD_LOAD_SAVESTATE: {
            // Server is sending us a savestate for desync recovery
            // Payload: uint32_t frame_num + uint32_t state_size + state_data
            if (hdr.size >= 8) {
                uint32_t frame_num = ntohl(*(uint32_t*)buf);
                uint32_t state_size = ntohl(*(uint32_t*)(buf + 4));

                if (state_size <= rb.state_size && hdr.size >= 8 + state_size) {
                    LOG_info("Rollback: loading savestate from server for frame %u (%u bytes)\n",
                             frame_num, state_size);
                    if (rb.unserialize_fn(buf + 8, state_size)) {
                        // Reset to this frame
                        rb.self_frame = frame_num;
                        rb.desync_detected = false;
                        snprintf(rb.status_msg, sizeof(rb.status_msg),
                                 "Resync from server (frame %u)", frame_num);
                    }
                }
            }
            break;
        }

        case RA_CMD_DISCONNECT: {
            LOG_info("Rollback: server disconnected\n");
            rb.connected = false;
            snprintf(rb.status_msg, sizeof(rb.status_msg), "Server disconnected");
            return oldest_wrong;
        }

        case RA_CMD_PAUSE: {
            snprintf(rb.status_msg, sizeof(rb.status_msg), "Server paused");
            break;
        }

        case RA_CMD_RESUME: {
            snprintf(rb.status_msg, sizeof(rb.status_msg), "Rollback active");
            break;
        }

        default:
            // Unknown command - ignore
            break;
        }
    }

    return oldest_wrong;
}

// Perform rollback: load state at `from_frame`, replay up to but not including `to_frame`
static void do_rollback(uint32_t from_frame, uint32_t to_frame) {
    if (from_frame >= to_frame) return;
    if (to_frame - from_frame > ROLLBACK_BUFFER_SIZE) {
        LOG_info("Rollback: too many frames to replay (%u), giving up\n",
                 to_frame - from_frame);
        return;
    }

    LOG_info("Rollback: rewinding from frame %u, replaying to %u (%u frames)\n",
             from_frame, to_frame, to_frame - from_frame);

    // Load the state at from_frame
    if (!load_state(from_frame)) {
        LOG_info("Rollback: failed to load state for frame %u\n", from_frame);
        return;
    }

    // Replay each frame from from_frame to to_frame - 1
    rb.replaying = true;
    for (uint32_t f = from_frame; f < to_frame; f++) {
        replay_frame = f;

        // Run the core for this frame (A/V suppressed via Rollback_isReplaying())
        rb.core_run_fn();

        // Re-save corrected state
        save_state(f + 1);  // State after running frame f = state at frame f+1

        // Compute CRC for corrected state if interval matches
        if (ROLLBACK_CRC_INTERVAL == 0 || (f % ROLLBACK_CRC_INTERVAL) == 0) {
            uint32_t next_idx = (f + 1) & ROLLBACK_BUFFER_MASK;
            if (rb.state_buffer[next_idx]) {
                get_slot(f + 1)->crc = compute_crc32(rb.state_buffer[next_idx], rb.state_size);
            }
        }
    }
    rb.replaying = false;
}

//////////////////////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////////////////////

int Rollback_init(int tcp_fd, uint32_t client_num, uint32_t start_frame,
                  Rollback_SerializeSizeFn serialize_size,
                  Rollback_SerializeFn serialize,
                  Rollback_UnserializeFn unserialize,
                  Rollback_CoreRunFn core_run) {

    memset(&rb, 0, sizeof(rb));
    pthread_mutex_init(&rb.mutex, NULL);

    rb.tcp_fd = tcp_fd;
    rb.client_num = client_num;
    rb.start_frame = start_frame;
    rb.self_frame = start_frame;
    rb.read_frame = start_frame;

    rb.serialize_size_fn = serialize_size;
    rb.serialize_fn = serialize;
    rb.unserialize_fn = unserialize;
    rb.core_run_fn = core_run;

    // Allocate state ring buffer
    rb.state_size = serialize_size();
    if (rb.state_size == 0) {
        LOG_info("Rollback: core serialize_size returned 0\n");
        return -1;
    }

    LOG_info("Rollback: state_size=%zu, ring buffer=%zu bytes total\n",
             rb.state_size, rb.state_size * ROLLBACK_BUFFER_SIZE);

    rb.state_buffer = calloc(ROLLBACK_BUFFER_SIZE, sizeof(void*));
    if (!rb.state_buffer) {
        LOG_info("Rollback: failed to allocate state buffer array\n");
        return -1;
    }

    for (int i = 0; i < ROLLBACK_BUFFER_SIZE; i++) {
        rb.state_buffer[i] = malloc(rb.state_size);
        if (!rb.state_buffer[i]) {
            LOG_info("Rollback: failed to allocate state slot %d\n", i);
            Rollback_quit();
            return -1;
        }
    }

    // Initialize frame slots
    for (int i = 0; i < ROLLBACK_BUFFER_SIZE; i++) {
        init_slot(i);
    }

    // Save initial state
    save_state(start_frame);

    rb.active = true;
    rb.connected = true;
    snprintf(rb.status_msg, sizeof(rb.status_msg), "Rollback active");

    LOG_info("Rollback: initialized (client=%u, start_frame=%u)\n", client_num, start_frame);
    return 0;
}

void Rollback_quit(void) {
    if (!rb.active && !rb.state_buffer) return;

    rb.active = false;
    rb.connected = false;

    if (rb.tcp_fd >= 0) {
        RA_sendCmd(rb.tcp_fd, RA_CMD_DISCONNECT, NULL, 0);
        close(rb.tcp_fd);
        rb.tcp_fd = -1;
    }

    if (rb.state_buffer) {
        for (int i = 0; i < ROLLBACK_BUFFER_SIZE; i++) {
            free(rb.state_buffer[i]);
        }
        free(rb.state_buffer);
        rb.state_buffer = NULL;
    }

    pthread_mutex_destroy(&rb.mutex);
    memset(&rb, 0, sizeof(rb));
    rb.tcp_fd = -1;
}

int Rollback_update(uint16_t local_input) {
    if (!rb.active || !rb.connected || rb.tcp_fd < 0) return 0;

    pthread_mutex_lock(&rb.mutex);

    //
    // 1. Initialize the slot for the current frame
    //
    RollbackFrameSlot* cur = get_slot(rb.self_frame);
    if (!cur->remote_confirmed) {
        // Predict remote input: copy last confirmed remote input
        uint16_t predicted = 0;
        if (rb.read_frame >= rb.start_frame) {
            predicted = get_slot(rb.read_frame)->remote_input;
        }
        cur->remote_input = predicted;
        cur->remote_confirmed = false;
    }
    cur->local_input = local_input;

    //
    // 2. Save state BEFORE running this frame
    //    (so we can rewind to this point if prediction is wrong)
    //
    save_state(rb.self_frame);

    //
    // 3. Send our input to the RA host
    //
    RA_sendInput(rb.tcp_fd, rb.self_frame, rb.client_num, local_input);

    //
    // 4. Process incoming data from the RA host
    //
    uint32_t oldest_wrong = process_incoming();

    //
    // 5. If any past prediction was wrong, rollback and replay
    //
    if (oldest_wrong != UINT32_MAX && oldest_wrong < rb.self_frame) {
        do_rollback(oldest_wrong, rb.self_frame);
    }

    //
    // 6. Send CRC for frames at the configured interval
    //
    if (ROLLBACK_CRC_INTERVAL == 0 ||
        (rb.self_frame % ROLLBACK_CRC_INTERVAL) == 0) {
        uint32_t idx = rb.self_frame & ROLLBACK_BUFFER_MASK;
        if (rb.state_buffer[idx] && get_slot(rb.self_frame)->state_saved) {
            uint32_t crc = compute_crc32(rb.state_buffer[idx], rb.state_size);
            get_slot(rb.self_frame)->crc = crc;
            RA_sendCRC(rb.tcp_fd, rb.self_frame, crc);
        }
    }

    pthread_mutex_unlock(&rb.mutex);

    // Rollback never stalls - always run the frame
    return 1;
}

void Rollback_postFrame(void) {
    if (!rb.active) return;

    pthread_mutex_lock(&rb.mutex);
    rb.self_frame++;

    // Initialize next slot
    init_slot(rb.self_frame);

    pthread_mutex_unlock(&rb.mutex);
}

uint16_t Rollback_getInput(unsigned port) {
    uint32_t frame;

    if (rb.replaying) {
        frame = replay_frame;
    } else {
        frame = rb.self_frame;
    }

    RollbackFrameSlot* slot = get_slot(frame);

    // Port 0 = host (player 1), Port 1 = us (client, player 2)
    // In RA's model, port mapping depends on client_num.
    // For a 2-player game: host = port 0, first client = port 1
    if (port == 0) {
        return slot->remote_input;   // Host's input
    } else {
        return slot->local_input;    // Our input
    }
}

bool Rollback_isReplaying(void) {
    return rb.replaying;
}

bool Rollback_isActive(void) {
    return rb.active && rb.connected;
}

bool Rollback_isConnected(void) {
    return rb.active && rb.connected && rb.tcp_fd >= 0;
}

const char* Rollback_getStatusMessage(void) {
    return rb.status_msg;
}

void Rollback_pause(void) {
    if (!rb.active || rb.tcp_fd < 0) return;
    RA_sendCmd(rb.tcp_fd, RA_CMD_PAUSE, NULL, 0);
    snprintf(rb.status_msg, sizeof(rb.status_msg), "Paused");
}

void Rollback_resume(void) {
    if (!rb.active || rb.tcp_fd < 0) return;
    RA_sendCmd(rb.tcp_fd, RA_CMD_RESUME, NULL, 0);
    snprintf(rb.status_msg, sizeof(rb.status_msg), "Rollback active");
}

void Rollback_disconnect(void) {
    if (rb.tcp_fd >= 0) {
        RA_sendCmd(rb.tcp_fd, RA_CMD_DISCONNECT, NULL, 0);
        close(rb.tcp_fd);
        rb.tcp_fd = -1;
    }
    rb.connected = false;
    rb.active = false;
    snprintf(rb.status_msg, sizeof(rb.status_msg), "Disconnected");
}
