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
#include <fcntl.h>
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

// Read exactly `size` bytes non-blocking (expects data to be available).
static bool recv_exact_nb(int fd, void* buf, size_t size) {
    uint8_t* ptr = (uint8_t*)buf;
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t ret = recv(fd, ptr, remaining, 0);
        if (ret <= 0) {
            if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            return false;
        }
        ptr += ret;
        remaining -= ret;
    }
    return true;
}

// Drain and discard `remaining` bytes from socket.
static bool drain_bytes(int fd, uint32_t remaining) {
    uint8_t tmp[256];
    while (remaining > 0) {
        uint32_t chunk = remaining < sizeof(tmp) ? remaining : sizeof(tmp);
        ssize_t ret = recv(fd, tmp, chunk, 0);
        if (ret <= 0) {
            if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            return false;
        }
        remaining -= ret;
    }
    return true;
}

// Read payload into buf (up to max_size), drain any excess.
static bool recv_payload(int fd, void* buf, uint32_t max_size, uint32_t payload_size) {
    if (payload_size == 0) return true;
    if (payload_size <= max_size) {
        return recv_exact_nb(fd, buf, payload_size);
    }
    // Read what fits, drain the rest
    if (!recv_exact_nb(fd, buf, max_size)) return false;
    return drain_bytes(fd, payload_size - max_size);
}

// Read an RA packet header from the socket (non-blocking).
// Returns true if header was read, fields are in host byte order.
static bool recv_ra_header(RA_PacketHeader* hdr) {
    if (!has_pending_data(rb.tcp_fd)) return false;
    if (!recv_exact_nb(rb.tcp_fd, hdr, sizeof(*hdr))) return false;
    hdr->cmd  = ntohl(hdr->cmd);
    hdr->size = ntohl(hdr->size);
    return true;
}

// Receive and process all pending RA commands. Returns the oldest frame
// where a prediction was wrong, or UINT32_MAX if all predictions were correct.
static uint32_t process_incoming(void) {
    uint32_t oldest_wrong = UINT32_MAX;

    while (rb.tcp_fd >= 0 && has_pending_data(rb.tcp_fd)) {
        // Read header (8 bytes) first, then decide how to receive the payload
        RA_PacketHeader hdr;
        if (!recv_ra_header(&hdr)) {
            break;
        }

        switch (hdr.cmd) {
        case RA_CMD_INPUT: {
            uint8_t buf[32];  // CMD_INPUT is small (~20 bytes)
            if (!recv_payload(rb.tcp_fd, buf, sizeof(buf), hdr.size)) break;

            uint32_t frame_num, player_num;
            uint16_t input;
            if (!RA_parseInput(buf, hdr.size, &frame_num, &player_num, &input)) {
                LOG_info("Rollback: failed to parse CMD_INPUT\n");
                break;
            }

            // Ignore our own input echoed back.
            // When client: our client_num != 0, remote (host) sends as 0
            // When server: our client_num == 0, remote (client) sends as 1
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
            if (frame_num > rb.read_frame || rb.read_frame == rb.start_frame) {
                rb.read_frame = frame_num;
            }
            break;
        }

        case RA_CMD_CRC: {
            uint8_t buf[8];
            if (!recv_payload(rb.tcp_fd, buf, sizeof(buf), hdr.size)) break;

            if (hdr.size >= 8) {
                uint32_t frame_num, server_crc;
                memcpy(&frame_num, buf, 4);
                memcpy(&server_crc, buf + 4, 4);
                frame_num = ntohl(frame_num);
                server_crc = ntohl(server_crc);
                RollbackFrameSlot* slot = get_slot(frame_num);

                // Compute local CRC on-demand if we have saved state but no CRC yet
                if (slot->crc == 0 && slot->state_saved) {
                    uint32_t idx = frame_num & ROLLBACK_BUFFER_MASK;
                    if (rb.state_buffer[idx]) {
                        slot->crc = compute_crc32(rb.state_buffer[idx], rb.state_size);
                    }
                }

                if (slot->crc != 0 && slot->crc != server_crc) {
                    LOG_info("Rollback: DESYNC at frame %u (local=0x%08x server=0x%08x)\n",
                             frame_num, slot->crc, server_crc);
                    rb.desync_detected = true;
                }
            }
            break;
        }

        case RA_CMD_LOAD_SAVESTATE: {
            // Server is sending us a savestate for desync recovery.
            // RA wire format: frame_num(4) + uncompressed_size(4) + data(cmd_size-8)
            // For nil compression: data = raw state bytes
            // Savestate can be very large (100KB+), so we receive it into a
            // dynamically allocated buffer, not the stack.
            if (hdr.size < 8) {
                drain_bytes(rb.tcp_fd, hdr.size);
                break;
            }

            // Read frame_num + uncompressed_size header (8 bytes)
            uint8_t ss_hdr[8];
            if (!recv_exact_nb(rb.tcp_fd, ss_hdr, 8)) break;
            uint32_t remaining_payload = hdr.size - 8;

            uint32_t frame_num, state_size;
            memcpy(&frame_num, ss_hdr, 4);
            memcpy(&state_size, ss_hdr + 4, 4);
            frame_num = ntohl(frame_num);
            state_size = ntohl(state_size);

            if (state_size > rb.state_size || state_size > remaining_payload) {
                LOG_info("Rollback: savestate size mismatch (%u vs %zu), draining\n",
                         state_size, rb.state_size);
                drain_bytes(rb.tcp_fd, remaining_payload);
                break;
            }

            // Allocate temp buffer for the savestate data
            void* ss_data = malloc(state_size);
            if (!ss_data) {
                LOG_info("Rollback: failed to allocate savestate buffer (%u bytes)\n", state_size);
                drain_bytes(rb.tcp_fd, remaining_payload);
                break;
            }

            if (!recv_exact_nb(rb.tcp_fd, ss_data, state_size)) {
                free(ss_data);
                break;
            }
            // Drain any remaining bytes after the state data
            if (remaining_payload > state_size) {
                drain_bytes(rb.tcp_fd, remaining_payload - state_size);
            }

            LOG_info("Rollback: loading savestate from server for frame %u (%u bytes)\n",
                     frame_num, state_size);
            if (rb.unserialize_fn(ss_data, state_size)) {
                rb.self_frame = frame_num;
                rb.desync_detected = false;
                snprintf(rb.status_msg, sizeof(rb.status_msg),
                         "Resync from server (frame %u)", frame_num);
            }
            free(ss_data);
            break;
        }

        case RA_CMD_DISCONNECT: {
            drain_bytes(rb.tcp_fd, hdr.size);
            LOG_info("Rollback: server disconnected\n");
            rb.connected = false;
            snprintf(rb.status_msg, sizeof(rb.status_msg), "Server disconnected");
            return oldest_wrong;
        }

        case RA_CMD_PAUSE: {
            drain_bytes(rb.tcp_fd, hdr.size);
            snprintf(rb.status_msg, sizeof(rb.status_msg), "Server paused");
            break;
        }

        case RA_CMD_RESUME: {
            drain_bytes(rb.tcp_fd, hdr.size);
            snprintf(rb.status_msg, sizeof(rb.status_msg), "Rollback active");
            break;
        }

        case RA_CMD_REQUEST_SAVESTATE: {
            // RA client requests savestate for desync recovery.
            // For now, ignore this — sending our core state crashes RA because
            // different snes9x versions produce incompatible savestate formats,
            // and our nil-compression path may not match RA's decompressor.
            // Both sides start from the same ROM at frame 0, so initial sync
            // is not needed. RA will continue without the savestate.
            drain_bytes(rb.tcp_fd, hdr.size);
            LOG_info("Rollback: client requested savestate (ignored - not supported yet)\n");
            break;
        }

        default:
            // Unknown command - drain payload and ignore
            LOG_info("Rollback: unknown cmd=0x%04x size=%u\n", hdr.cmd, hdr.size);
            drain_bytes(rb.tcp_fd, hdr.size);
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
                  bool is_server,
                  Rollback_SerializeSizeFn serialize_size,
                  Rollback_SerializeFn serialize,
                  Rollback_UnserializeFn unserialize,
                  Rollback_CoreRunFn core_run) {

    memset(&rb, 0, sizeof(rb));
    pthread_mutex_init(&rb.mutex, NULL);

    // Don't set rb.tcp_fd yet - we don't own it until all allocations succeed.
    // If Rollback_quit() is called during init failure, it would close a fd
    // that the caller (lockstep) still owns.
    rb.tcp_fd = -1;
    rb.client_num = client_num;
    rb.is_server = is_server;
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

    // Now take ownership of the TCP fd (all allocations succeeded)
    // Set non-blocking mode — the accepted socket from listen thread is
    // blocking by default, but our recv loop needs non-blocking I/O.
    int flags = fcntl(tcp_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(tcp_fd, F_SETFL, flags | O_NONBLOCK);
    rb.tcp_fd = tcp_fd;

    // Save initial state
    save_state(start_frame);

    rb.active = true;
    rb.connected = true;
    rb.force_send_savestate = false;  // Don't send initial savestate — RA starts from same ROM state
    snprintf(rb.status_msg, sizeof(rb.status_msg), "Rollback active");

    LOG_info("Rollback: initialized (%s, client=%u, start_frame=%u)\n",
             is_server ? "server" : "client", client_num, start_frame);
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

    if (rb.pause_state) {
        free(rb.pause_state);
        rb.pause_state = NULL;
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
    if (!rb.active || !rb.connected || rb.tcp_fd < 0) {
        LOG_info("Rollback_update: early exit (active=%d connected=%d fd=%d)\n",
                 rb.active, rb.connected, rb.tcp_fd);
        return 0;
    }

    pthread_mutex_lock(&rb.mutex);

    //
    // 0a. Server: send initial savestate to sync RA client to our game state.
    //     Deferred from handshake to here because RA hasn't finished its
    //     post-handshake initialization (buffer allocation) during the handshake
    //     itself. RA's own server does this via force_send_savestate on the
    //     first frame loop iteration.
    //
    if (rb.force_send_savestate) {
        rb.force_send_savestate = false;

        // Check if connection is still alive before sending large savestate
        int sock_err = 0;
        socklen_t sock_err_len = sizeof(sock_err);
        getsockopt(rb.tcp_fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len);
        if (sock_err != 0) {
            LOG_info("Rollback: socket error %d before savestate send, skipping\n", sock_err);
        } else {
            size_t ss_size = rb.serialize_size_fn();
            if (ss_size > 0) {
                void* ss_data = malloc(ss_size);
                if (ss_data) {
                    if (rb.serialize_fn(ss_data, ss_size)) {
                        LOG_info("Rollback: sending initial savestate (%zu bytes, frame %u)\n",
                                 ss_size, rb.self_frame);
                        if (!RA_sendSavestate(rb.tcp_fd, rb.self_frame, ss_data, ss_size)) {
                            LOG_info("Rollback: savestate send failed, disconnecting\n");
                            rb.connected = false;
                        }
                    } else {
                        LOG_info("Rollback: failed to serialize initial savestate\n");
                    }
                    free(ss_data);
                }
            }
        }
    }

    //
    // 0. Enforce max-ahead limit: don't let the client run more than
    //    ROLLBACK_MAX_AHEAD frames past the last confirmed remote input.
    //    This prevents ring buffer wraparound from overwriting unconfirmed slots.
    //    If we're too far ahead, poll for incoming data before continuing.
    //    After resume, a grace period skips this check to prevent deadlock.
    //
    if (rb.resume_grace_frames > 0) {
        rb.resume_grace_frames--;
        if (rb.resume_grace_frames % 10 == 0) {
            LOG_info("Rollback: grace=%u self=%u read=%u\n",
                     rb.resume_grace_frames, rb.self_frame, rb.read_frame);
        }
    }
    if (rb.resume_grace_frames == 0 &&
        rb.self_frame > rb.read_frame + ROLLBACK_MAX_AHEAD) {
        // Try to receive more remote inputs, waiting up to 16ms (one frame)
        // to avoid busy-spinning when the remote side is just slightly behind.
        for (int wait_i = 0; wait_i < 4; wait_i++) {
            process_incoming();
            if (!rb.connected) {
                LOG_info("Rollback: disconnected during max-ahead poll\n");
                pthread_mutex_unlock(&rb.mutex);
                return 0;
            }
            if (rb.self_frame <= rb.read_frame + ROLLBACK_MAX_AHEAD) break;
            // Brief wait (4ms) for more data before retrying
            if (wait_i < 3) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(rb.tcp_fd, &fds);
                struct timeval tv = {0, 4000};  // 4ms
                select(rb.tcp_fd + 1, &fds, NULL, NULL, &tv);
            }
        }
        // If still too far ahead after waiting, skip this frame (stall)
        if (rb.self_frame > rb.read_frame + ROLLBACK_MAX_AHEAD) {
            static uint32_t stall_log_count = 0;
            if (stall_log_count++ < 10) {
                LOG_info("Rollback: STALL self=%u read=%u ahead=%u\n",
                         rb.self_frame, rb.read_frame,
                         rb.self_frame - rb.read_frame);
            }
            pthread_mutex_unlock(&rb.mutex);
            return 0;
        }
    }

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
    // 3. Send our input to the remote peer
    //
    if (rb.is_server) {
        RA_sendServerInput(rb.tcp_fd, rb.self_frame, rb.client_num, local_input);
    } else {
        RA_sendInput(rb.tcp_fd, rb.self_frame, rb.client_num, local_input);
    }

    //
    // 4. Process incoming data from the RA host
    //
    uint32_t oldest_wrong = process_incoming();

    // Check if we got disconnected during processing
    if (!rb.connected) {
        pthread_mutex_unlock(&rb.mutex);
        return 0;
    }

    //
    // 5. If any past prediction was wrong, rollback and replay
    //
    if (oldest_wrong != UINT32_MAX && oldest_wrong < rb.self_frame) {
        do_rollback(oldest_wrong, rb.self_frame);
    }

    //
    // 6. CRC for desync detection
    //
    // In RA protocol, only the server sends CMD_CRC. Clients compute CRC
    // locally and compare when they receive the server's CRC.
    //
    if (ROLLBACK_CRC_INTERVAL == 0 ||
        (rb.self_frame % ROLLBACK_CRC_INTERVAL) == 0) {
        uint32_t idx = rb.self_frame & ROLLBACK_BUFFER_MASK;
        if (rb.state_buffer[idx] && get_slot(rb.self_frame)->state_saved) {
            uint32_t crc = compute_crc32(rb.state_buffer[idx], rb.state_size);
            get_slot(rb.self_frame)->crc = crc;

            // Server sends CRC to client for verification
            if (rb.is_server) {
                RA_sendCRC(rb.tcp_fd, rb.self_frame, crc);
            }
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

    // Port mapping depends on whether we're server or client.
    // Port 0 = player 1 (always host), Port 1 = player 2 (always client)
    if (rb.is_server) {
        // We are the host: port 0 = our local input, port 1 = remote (client) input
        if (port == 0) {
            return slot->local_input;
        } else {
            return slot->remote_input;
        }
    } else {
        // We are the client: port 0 = remote (host) input, port 1 = our local input
        if (port == 0) {
            return slot->remote_input;
        } else {
            return slot->local_input;
        }
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

bool Rollback_isServer(void) {
    return rb.active && rb.is_server;
}

const char* Rollback_getStatusMessage(void) {
    return rb.status_msg;
}

void Rollback_pause(void) {
    if (!rb.active || rb.tcp_fd < 0) return;
    LOG_info("Rollback_pause: self=%u read=%u connected=%d\n",
             rb.self_frame, rb.read_frame, rb.connected);
    rb.local_paused = true;

    // Save core state snapshot so we can replay on resume.
    // During pause, we send keepalive CMD_INPUT (0 buttons) to prevent RA's
    // stall timeout, but the core doesn't run. On resume, we restore this
    // snapshot and replay all accumulated frames to re-sync with RA.
    rb.pause_start_frame = rb.self_frame;
    if (rb.pause_state) { free(rb.pause_state); rb.pause_state = NULL; }
    rb.pause_state = malloc(rb.state_size);
    if (rb.pause_state) {
        if (!rb.serialize_fn(rb.pause_state, rb.state_size)) {
            LOG_info("Rollback: WARNING - failed to save pause state\n");
            free(rb.pause_state);
            rb.pause_state = NULL;
        }
    }

    // NOTE: We intentionally do NOT send CMD_PAUSE to RA.
    // RA's remote_unpaused() has a bug where operator precedence causes
    // remote_paused to be re-set to true after CMD_RESUME, making resume
    // impossible. Instead, we send keepalive CMD_INPUT during pollWhilePaused.
    snprintf(rb.status_msg, sizeof(rb.status_msg), "Paused");
}

void Rollback_resume(void) {
    if (!rb.active || rb.tcp_fd < 0) return;

    // Drain any remaining RA input that arrived during the pause
    process_incoming();

    rb.local_paused = false;

    uint32_t pause_frames = rb.self_frame - rb.pause_start_frame;
    LOG_info("Rollback: resumed self=%u read=%u pause_start=%u pause_frames=%u\n",
             rb.self_frame, rb.read_frame, rb.pause_start_frame, pause_frames);

    // Replay: restore core state from pause snapshot and re-run all frames
    // that RA ran during the pause. During pause we sent CMD_INPUT(0) to keep
    // RA alive, and RA sent us its real inputs. Now we run our core through
    // those same frames so both sides converge on the same state.
    if (rb.pause_state && pause_frames > 0) {
        // Restore core to the exact state at pause start
        if (!rb.unserialize_fn(rb.pause_state, rb.state_size)) {
            LOG_info("Rollback: WARNING - failed to restore pause state\n");
        } else {
            // Cap replay to ring buffer size (oldest frames beyond this are lost)
            uint32_t replay_start = rb.pause_start_frame;
            if (pause_frames > ROLLBACK_BUFFER_SIZE) {
                LOG_info("Rollback: pause was %u frames, capping replay to %u\n",
                         pause_frames, ROLLBACK_BUFFER_SIZE);
                // We lost the oldest frames' remote input, skip them
                // (small desync, but better than nothing)
                replay_start = rb.self_frame - ROLLBACK_BUFFER_SIZE;
            }

            uint32_t replay_count = rb.self_frame - replay_start;
            LOG_info("Rollback: replaying %u pause frames (%u -> %u)\n",
                     replay_count, replay_start, rb.self_frame);

            rb.replaying = true;
            for (uint32_t f = replay_start; f < rb.self_frame; f++) {
                replay_frame = f;
                RollbackFrameSlot* slot = get_slot(f);
                // local_input was set to 0 during pollWhilePaused
                // remote_input was set by process_incoming() during pollWhilePaused
                rb.core_run_fn();
            }
            rb.replaying = false;

            LOG_info("Rollback: pause replay complete at frame %u\n", rb.self_frame);
        }
    } else if (!rb.pause_state && pause_frames > 0) {
        LOG_info("Rollback: WARNING - no pause state, skipping replay (%u frames lost)\n",
                 pause_frames);
    }

    // Clean up pause state
    if (rb.pause_state) {
        free(rb.pause_state);
        rb.pause_state = NULL;
    }

    // Grace period: skip max-ahead stall to give RA time to unstall
    rb.resume_grace_frames = 30;

    snprintf(rb.status_msg, sizeof(rb.status_msg), "Rollback active");
}

bool Rollback_isPaused(void) {
    return rb.active && rb.local_paused;
}

void Rollback_pollWhilePaused(void) {
    // During menu/pause, drain incoming commands and keep RA alive.
    // Since we don't send CMD_PAUSE (RA remote_unpaused() bug workaround),
    // RA will enter RUNNING_FAST stall when it runs ahead of our last
    // sent frame. To prevent RA's ~5 second stall timeout from disconnecting,
    // we keep sending CMD_INPUT with incrementing frame numbers (0 input).
    // This advances connected_players_frame on RA's side, preventing stall.
    // On resume, we restore pause snapshot and replay all frames.
    if (!rb.active || rb.tcp_fd < 0 || !rb.connected) return;

    process_incoming();

    // Send a keep-alive CMD_INPUT every call to prevent RA stall timeout.
    // Use 0 input (no buttons) since the host is in the menu.
    if (rb.is_server) {
        RA_sendServerInput(rb.tcp_fd, rb.self_frame, rb.client_num, 0);
    } else {
        RA_sendInput(rb.tcp_fd, rb.self_frame, rb.client_num, 0);
    }

    // Initialize slot and record 0 input (core doesn't run during pause).
    // Don't save_state — core isn't running, state hasn't changed.
    // On resume, we'll restore from pause_state and replay.
    RollbackFrameSlot* slot = get_slot(rb.self_frame);
    slot->local_input = 0;
    slot->state_saved = false;
    rb.self_frame++;
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
