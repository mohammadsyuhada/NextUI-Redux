/*
 * RetroArch Netplay Protocol Module
 * Implements RA wire protocol for client-side compatibility.
 */

#include "ra_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Logging macro (uses NextUI LOG_info if available, falls back to fprintf)
#ifndef LOG_info
#define LOG_info(...) fprintf(stderr, __VA_ARGS__)
#endif

//////////////////////////////////////////////////////////////////////////////
// Internal helpers
//////////////////////////////////////////////////////////////////////////////

// Receive exactly `size` bytes with timeout. Returns true on success.
// The timeout covers the entire operation, not each individual recv call.
static bool recv_exact(int fd, void* buf, size_t size, int timeout_ms) {
    uint8_t* ptr = (uint8_t*)buf;
    size_t remaining = size;

    struct timeval deadline = {0, 0};
    if (timeout_ms > 0) {
        gettimeofday(&deadline, NULL);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_usec += (timeout_ms % 1000) * 1000;
        if (deadline.tv_usec >= 1000000) {
            deadline.tv_sec++;
            deadline.tv_usec -= 1000000;
        }
    }

    while (remaining > 0) {
        if (timeout_ms > 0) {
            struct timeval now, tv;
            gettimeofday(&now, NULL);
            tv.tv_sec  = deadline.tv_sec  - now.tv_sec;
            tv.tv_usec = deadline.tv_usec - now.tv_usec;
            if (tv.tv_usec < 0) {
                tv.tv_sec--;
                tv.tv_usec += 1000000;
            }
            if (tv.tv_sec < 0) return false;  // Deadline passed

            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            int sel = select(fd + 1, &fds, NULL, NULL, &tv);
            if (sel <= 0) return false;
        }

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

// Send exactly `size` bytes. Returns true on success.
static bool send_exact(int fd, const void* buf, size_t size) {
    const uint8_t* ptr = (const uint8_t*)buf;
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t ret = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (ret <= 0) {
            if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            LOG_info("RA: send_exact failed: ret=%zd, errno=%d (%s), remaining=%zu/%zu\n",
                     ret, errno, strerror(errno), remaining, size);
            return false;
        }
        ptr += ret;
        remaining -= ret;
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////////////////////

bool RA_sendCmd(int fd, uint32_t cmd, const void* data, uint32_t size) {
    RA_PacketHeader hdr = {
        .cmd  = htonl(cmd),
        .size = htonl(size)
    };

    if (!send_exact(fd, &hdr, sizeof(hdr))) return false;
    if (size > 0 && data) {
        if (!send_exact(fd, data, size)) return false;
    }
    return true;
}

bool RA_recvCmd(int fd, RA_PacketHeader* hdr, void* data, uint32_t max_size, int timeout_ms) {
    if (!recv_exact(fd, hdr, sizeof(*hdr), timeout_ms)) return false;

    hdr->cmd  = ntohl(hdr->cmd);
    hdr->size = ntohl(hdr->size);

    if (hdr->size > 0) {
        if (data && hdr->size <= max_size) {
            if (!recv_exact(fd, data, hdr->size, timeout_ms)) return false;
        } else if (data && hdr->size > max_size) {
            // Read what we can, drain the rest
            if (!recv_exact(fd, data, max_size, timeout_ms)) return false;
            if (!RA_drainBytes(fd, hdr->size - max_size)) return false;
        } else {
            // No data buffer, drain payload
            if (!RA_drainBytes(fd, hdr->size)) return false;
        }
    }

    return true;
}

bool RA_drainBytes(int fd, uint32_t remaining) {
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

bool RA_sendServerInput(int fd, uint32_t frame_num, uint32_t client_num, uint16_t input) {
    // Same as RA_sendInput but with is_server bit set (bit 31)
    uint32_t payload[3];
    payload[0] = htonl(frame_num);
    payload[1] = htonl((1U << 31) | (client_num & 0x7FFFFFFF));
    payload[2] = htonl((uint32_t)input);

    return RA_sendCmd(fd, RA_CMD_INPUT, payload, sizeof(payload));
}

bool RA_sendInput(int fd, uint32_t frame_num, uint32_t client_num, uint16_t input) {
    // RA protocol CMD_INPUT payload (protocol v6):
    //   uint32_t frame_num
    //   uint32_t (is_server << 31) | (client_num & 0x7FFFFFFF)
    //   uint32_t input_word[N] — one word per device this player controls
    //
    // For RETRO_DEVICE_JOYPAD: N=1 (just the digital button bitmask).
    // The payload MUST be exactly (2 + N) * 4 bytes. RA computes N from
    // the payload size: N = (size/4) - 2. Sending extra words (e.g. analog
    // zeros) causes RA to overflow the per-device input buffer and crash.
    uint32_t payload[3];
    payload[0] = htonl(frame_num);
    payload[1] = htonl(client_num & 0x7FFFFFFF);  // is_server = 0 for client
    payload[2] = htonl((uint32_t)input);           // 1 joypad input word

    return RA_sendCmd(fd, RA_CMD_INPUT, payload, sizeof(payload));
}

bool RA_parseInput(const void* data, uint32_t size,
                   uint32_t* frame_out, uint32_t* player_out, uint16_t* input_out) {
    // RA protocol CMD_INPUT payload (protocol v6):
    //   uint32_t frame_num
    //   uint32_t (is_server << 31) | (player & 0x7FFFFFFF)
    //   uint32_t controller_input (RETRO_DEVICE_JOYPAD bitmask)
    //   uint32_t analog1 (optional)
    //   uint32_t analog2 (optional)
    if (size < 12) return false;  // Minimum: frame(4) + server|player(4) + input(4)

    const uint32_t* p = (const uint32_t*)data;
    *frame_out = ntohl(p[0]);

    uint32_t server_player = ntohl(p[1]);
    *player_out = server_player & 0x7FFFFFFF;  // Strip is_server bit

    *input_out = (uint16_t)ntohl(p[2]);  // Joypad state in low 16 bits
    return true;
}

bool RA_sendSavestate(int fd, uint32_t frame_num, const void* state_data, size_t state_size) {
    if (fd < 0 || !state_data || state_size == 0) return false;

    // RA CMD_LOAD_SAVESTATE wire format:
    //   CMD header: cmd(4) + payload_size(4)
    //   Payload: frame_num(4) + uncompressed_size(4) + data(state_size)
    // Total payload = 8 + state_size (NO separate compressed_size field)
    // For uncompressed (nil compression): data = raw state, RA decompresses with nil backend
    size_t payload_size = 8 + state_size;
    uint8_t* payload = malloc(payload_size);
    if (!payload) {
        LOG_info("RA: failed to allocate savestate payload (%zu bytes)\n", payload_size);
        return false;
    }

    uint32_t frame_be      = htonl(frame_num);
    uint32_t orig_size_be  = htonl((uint32_t)state_size);
    memcpy(payload,      &frame_be, 4);
    memcpy(payload + 4,  &orig_size_be, 4);
    memcpy(payload + 8,  state_data, state_size);

    bool ok = RA_sendCmd(fd, RA_CMD_LOAD_SAVESTATE, payload, (uint32_t)payload_size);
    free(payload);

    if (ok) {
        LOG_info("RA: sent savestate for frame %u (%zu bytes)\n", frame_num, state_size);
    } else {
        LOG_info("RA: failed to send savestate\n");
    }
    return ok;
}

bool RA_sendCRC(int fd, uint32_t frame_num, uint32_t crc) {
    RA_CRCPayload payload = {
        .frame_num = htonl(frame_num),
        .crc       = htonl(crc)
    };
    return RA_sendCmd(fd, RA_CMD_CRC, &payload, sizeof(payload));
}

//////////////////////////////////////////////////////////////////////////////
// Client Handshake
//////////////////////////////////////////////////////////////////////////////

int RA_clientHandshake(RA_HandshakeCtx* ctx) {
    if (ctx->tcp_fd < 0) return -1;

    int fd = ctx->tcp_fd;

    //
    // Step 1: Send client connection header
    //
    RA_ClientHeader client_hdr = {
        .magic          = htonl(RA_MAGIC),
        .platform_magic = htonl(RA_PLATFORM_MAGIC),
        .compression    = htonl(0),
        .proto_hi       = htonl(RA_PROTOCOL_VERSION_MAX),
        .proto_lo       = htonl(RA_PROTOCOL_VERSION_MIN),
        .impl_magic     = htonl(RA_IMPL_MAGIC)
    };

    if (!send_exact(fd, &client_hdr, sizeof(client_hdr))) {
        LOG_info("RA handshake: failed to send client header\n");
        return -1;
    }

    //
    // Step 2: Receive server connection header
    //
    RA_ServerHeader server_hdr;
    if (!recv_exact(fd, &server_hdr, sizeof(server_hdr), 10000)) {
        LOG_info("RA handshake: failed to receive server header\n");
        return -1;
    }

    if (ntohl(server_hdr.magic) != RA_MAGIC) {
        LOG_info("RA handshake: bad server magic 0x%08x\n", ntohl(server_hdr.magic));
        return -1;
    }

    ctx->negotiated_proto = ntohl(server_hdr.proto);
    LOG_info("RA handshake: server proto=%u, compression=%u\n",
             ctx->negotiated_proto, ntohl(server_hdr.compression));

    if (ctx->negotiated_proto < RA_PROTOCOL_VERSION_MIN ||
        ctx->negotiated_proto > RA_PROTOCOL_VERSION_MAX) {
        LOG_info("RA handshake: unsupported protocol version %u\n", ctx->negotiated_proto);
        return -1;
    }

    // Check if server requires a password (salt != 0)
    if (ntohl(server_hdr.salt) != 0) {
        LOG_info("RA handshake: server requires password (not supported)\n");
        return -1;
    }

    //
    // Step 3: Exchange CMD_NICK
    //
    // Send our nickname
    char nick_buf[RA_NICK_LEN];
    memset(nick_buf, 0, sizeof(nick_buf));
    strncpy(nick_buf, ctx->nick, RA_NICK_LEN - 1);

    if (!RA_sendCmd(fd, RA_CMD_NICK, nick_buf, RA_NICK_LEN)) {
        LOG_info("RA handshake: failed to send NICK\n");
        return -1;
    }

    // Receive server's nickname
    RA_PacketHeader hdr;
    char recv_nick[RA_NICK_LEN];
    if (!RA_recvCmd(fd, &hdr, recv_nick, sizeof(recv_nick), 10000)) {
        LOG_info("RA handshake: failed to receive server NICK\n");
        return -1;
    }
    if (hdr.cmd != RA_CMD_NICK) {
        LOG_info("RA handshake: expected NICK (0x%04x), got 0x%04x\n", RA_CMD_NICK, hdr.cmd);
        return -1;
    }
    memcpy(ctx->server_nick, recv_nick, RA_NICK_LEN);
    ctx->server_nick[RA_NICK_LEN - 1] = '\0';
    LOG_info("RA handshake: server nick = '%s'\n", ctx->server_nick);

    //
    // Step 4: Receive CMD_INFO from server
    //
    // The RA server sends its game info (core name, CRC) after the NICK exchange.
    // We must receive and acknowledge it before sending our own INFO.
    //
    RA_PacketHeader info_hdr;
    uint8_t info_recv_buf[256];
    if (!RA_recvCmd(fd, &info_hdr, info_recv_buf, sizeof(info_recv_buf), 10000)) {
        LOG_info("RA handshake: failed to receive server INFO\n");
        return -1;
    }
    if (info_hdr.cmd != RA_CMD_INFO) {
        LOG_info("RA handshake: expected INFO (0x%04x), got 0x%04x\n", RA_CMD_INFO, info_hdr.cmd);
        return -1;
    }
    LOG_info("RA handshake: received server INFO (%u bytes)\n", info_hdr.size);

    //
    // Step 5: Send CMD_INFO
    //
    RA_InfoPayload info;
    memset(&info, 0, sizeof(info));
    info.content_crc = htonl(ctx->content_crc);
    strncpy(info.core_name, ctx->core_name, RA_CORE_NAME_LEN - 1);
    strncpy(info.core_version, ctx->core_version, RA_CORE_VERSION_LEN - 1);

    if (!RA_sendCmd(fd, RA_CMD_INFO, &info, sizeof(info))) {
        LOG_info("RA handshake: failed to send INFO\n");
        return -1;
    }

    //
    // Step 6: Receive CMD_SYNC
    //
    // CMD_SYNC payload is variable-length. We read it into a buffer and parse key fields.
    // Minimum fields: frame_num(4) + connections(4) + client_num(4) = 12 bytes
    // Plus share_modes (16*4=64) + per-client data (variable)
    uint8_t sync_buf[4096];
    if (!RA_recvCmd(fd, &hdr, sync_buf, sizeof(sync_buf), 10000)) {
        LOG_info("RA handshake: failed to receive SYNC\n");
        return -1;
    }
    if (hdr.cmd != RA_CMD_SYNC) {
        LOG_info("RA handshake: expected SYNC (0x%04x), got 0x%04x\n", RA_CMD_SYNC, hdr.cmd);
        return -1;
    }

    if (hdr.size < 12) {
        LOG_info("RA handshake: SYNC payload too small (%u bytes)\n", hdr.size);
        return -1;
    }

    // Parse key fields from SYNC payload
    const uint32_t* sync_words = (const uint32_t*)sync_buf;
    ctx->start_frame = ntohl(sync_words[0]);
    // sync_words[1] = connections bitmask
    ctx->client_num  = ntohl(sync_words[2]);

    LOG_info("RA handshake: start_frame=%u, client_num=%u\n",
             ctx->start_frame, ctx->client_num);

    //
    // Step 7: Send CMD_PLAY (request a player slot)
    //
    // After SYNC, the client must request to play. Without this, the server
    // treats us as a spectator and crashes when we send CMD_INPUT.
    // Payload: uint32_t with bit 31 = slave flag, bits 16-23 = share mode,
    // bits 0-15 = requested devices. 0 = auto-assign.
    uint32_t play_request = htonl(0);  // Auto-assign device
    if (!RA_sendCmd(fd, RA_CMD_PLAY, &play_request, sizeof(play_request))) {
        LOG_info("RA handshake: failed to send PLAY\n");
        return -1;
    }

    //
    // Step 8: Wait for CMD_MODE response (player assignment confirmation)
    //
    // The server responds with CMD_MODE (60 bytes) containing our assignment.
    // It may send other commands (INPUT, CRC) before MODE, so we loop.
    // MODE payload: uint32_t frame, uint32_t mode_flags, uint32_t devices,
    //               uint8_t share_modes[16], char nick[32]
    // mode_flags: bit 31 = YOU, bit 30 = PLAYING, bit 29 = SLAVE, bits 0-15 = client_num
    bool got_mode = false;
    for (int attempts = 0; attempts < 50 && !got_mode; attempts++) {
        RA_PacketHeader mode_hdr;
        uint8_t mode_buf[64];  // CMD_MODE is 60 bytes
        if (!RA_recvCmd(fd, &mode_hdr, mode_buf, sizeof(mode_buf), 10000)) {
            LOG_info("RA handshake: timeout waiting for MODE\n");
            return -1;
        }

        if (mode_hdr.cmd == RA_CMD_MODE && mode_hdr.size >= 8) {
            uint32_t mode_frame, mode_flags;
            memcpy(&mode_frame, mode_buf, 4);
            memcpy(&mode_flags, mode_buf + 4, 4);
            mode_frame = ntohl(mode_frame);
            mode_flags = ntohl(mode_flags);

            // Check YOU bit (bit 31) - this MODE is addressed to us
            if (mode_flags & (1U << 31)) {
                // Check PLAYING bit (bit 30)
                if (mode_flags & (1U << 30)) {
                    uint32_t assigned_client = mode_flags & 0xFFFF;
                    LOG_info("RA handshake: MODE - playing as client %u, start frame=%u\n",
                             assigned_client, mode_frame);
                    // Server may provide a later start frame for our input
                    if (mode_frame > ctx->start_frame) {
                        ctx->start_frame = mode_frame;
                    }
                    got_mode = true;
                } else {
                    LOG_info("RA handshake: MODE - play request refused\n");
                    return -1;
                }
            }
            // MODE without YOU bit is for another client - ignore
        }
        // Non-MODE commands are consumed by RA_recvCmd and discarded
    }

    if (!got_mode) {
        LOG_info("RA handshake: never received MODE confirmation\n");
        return -1;
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// Server Handshake
//////////////////////////////////////////////////////////////////////////////

int RA_serverHandshake(RA_ServerHandshakeCtx* ctx) {
    if (ctx->tcp_fd < 0) return -1;

    int fd = ctx->tcp_fd;

    //
    // Step 1: Receive client connection header
    //
    RA_ClientHeader client_hdr;
    if (!recv_exact(fd, &client_hdr, sizeof(client_hdr), 10000)) {
        LOG_info("RA server: failed to receive client header\n");
        return -1;
    }

    if (ntohl(client_hdr.magic) != RA_MAGIC) {
        LOG_info("RA server: bad client magic 0x%08x\n", ntohl(client_hdr.magic));
        return -1;
    }

    uint32_t client_proto_hi = ntohl(client_hdr.proto_hi);
    uint32_t client_proto_lo = ntohl(client_hdr.proto_lo);
    LOG_info("RA server: client proto range [%u, %u], compression=%u\n",
             client_proto_lo, client_proto_hi, ntohl(client_hdr.compression));

    // Negotiate protocol version
    if (client_proto_hi < RA_PROTOCOL_VERSION_MIN || client_proto_lo > RA_PROTOCOL_VERSION_MAX) {
        LOG_info("RA server: no compatible protocol version\n");
        return -1;
    }
    ctx->negotiated_proto = RA_PROTOCOL_VERSION;

    //
    // Step 2: Send server connection header
    //
    RA_ServerHeader server_hdr = {
        .magic          = htonl(RA_MAGIC),
        .platform_magic = htonl(RA_PLATFORM_MAGIC),
        .compression    = htonl(0),         // No compression
        .salt           = htonl(0),         // No password
        .proto          = htonl(ctx->negotiated_proto),
        .impl_magic     = htonl(RA_IMPL_MAGIC)
    };

    if (!send_exact(fd, &server_hdr, sizeof(server_hdr))) {
        LOG_info("RA server: failed to send server header\n");
        return -1;
    }

    //
    // Step 3: Receive CMD_NICK from client
    //
    RA_PacketHeader hdr;
    char recv_nick[RA_NICK_LEN];
    if (!RA_recvCmd(fd, &hdr, recv_nick, sizeof(recv_nick), 10000)) {
        LOG_info("RA server: failed to receive client NICK\n");
        return -1;
    }
    if (hdr.cmd != RA_CMD_NICK) {
        LOG_info("RA server: expected NICK (0x%04x), got 0x%04x\n", RA_CMD_NICK, hdr.cmd);
        return -1;
    }
    memcpy(ctx->client_nick, recv_nick, RA_NICK_LEN);
    ctx->client_nick[RA_NICK_LEN - 1] = '\0';
    LOG_info("RA server: client nick = '%s'\n", ctx->client_nick);

    //
    // Step 4: Send CMD_NICK
    //
    char nick_buf[RA_NICK_LEN];
    memset(nick_buf, 0, sizeof(nick_buf));
    strncpy(nick_buf, ctx->nick, RA_NICK_LEN - 1);

    if (!RA_sendCmd(fd, RA_CMD_NICK, nick_buf, RA_NICK_LEN)) {
        LOG_info("RA server: failed to send NICK\n");
        return -1;
    }

    //
    // Step 5: Send CMD_INFO
    //
    RA_InfoPayload info;
    memset(&info, 0, sizeof(info));
    info.content_crc = htonl(ctx->content_crc);
    strncpy(info.core_name, ctx->core_name, RA_CORE_NAME_LEN - 1);
    strncpy(info.core_version, ctx->core_version, RA_CORE_VERSION_LEN - 1);

    if (!RA_sendCmd(fd, RA_CMD_INFO, &info, sizeof(info))) {
        LOG_info("RA server: failed to send INFO\n");
        return -1;
    }

    //
    // Step 6: Receive CMD_INFO from client
    //
    RA_PacketHeader info_hdr;
    uint8_t info_recv_buf[256];
    if (!RA_recvCmd(fd, &info_hdr, info_recv_buf, sizeof(info_recv_buf), 10000)) {
        LOG_info("RA server: failed to receive client INFO\n");
        return -1;
    }
    if (info_hdr.cmd != RA_CMD_INFO) {
        LOG_info("RA server: expected INFO (0x%04x), got 0x%04x\n", RA_CMD_INFO, info_hdr.cmd);
        return -1;
    }
    LOG_info("RA server: received client INFO (%u bytes)\n", info_hdr.size);

    // Optionally verify CRC match
    if (info_hdr.size >= sizeof(RA_InfoPayload)) {
        RA_InfoPayload* client_info = (RA_InfoPayload*)info_recv_buf;
        uint32_t client_crc = ntohl(client_info->content_crc);
        if (client_crc != ctx->content_crc) {
            LOG_info("RA server: WARNING - CRC mismatch (ours=0x%08x, client=0x%08x)\n",
                     ctx->content_crc, client_crc);
        }
    }

    //
    // Step 7: Send CMD_SYNC
    //
    // CMD_SYNC payload format (184 bytes, no SRAM):
    //   uint32_t frame_num                              offset 0
    //   uint32_t client_num (bit31=paused flag)         offset 4
    //   uint32_t config_devices[16]                     offset 8
    //   uint8_t  device_share_modes[16]                 offset 72
    //   uint32_t device_clients[16]                     offset 88
    //   char     nick[32]   (RA_NICK_LEN)              offset 152
    //
    // Total: 4 + 4 + 64 + 16 + 64 + 32 = 184 bytes
    // NETPLAY_NICK_LEN = 32 in RA (not 128)
    uint8_t sync_buf[184];
    memset(sync_buf, 0, sizeof(sync_buf));
    uint32_t* sync_words = (uint32_t*)sync_buf;

    ctx->client_num = 1;  // First client is always 1

    sync_words[0] = htonl(ctx->start_frame);     // frame_num
    sync_words[1] = htonl(ctx->client_num);       // client_num (bit31=0, not paused)

    // config_devices[16]: device 0 = JOYPAD(1), device 1 = JOYPAD(1), rest = NONE(0)
    sync_words[2] = htonl(1);  // devices[0] = RETRO_DEVICE_JOYPAD
    sync_words[3] = htonl(1);  // devices[1] = RETRO_DEVICE_JOYPAD

    // device_share_modes[16] at offset 72: all zeros (already memset)

    // device_clients[16] at offset 88:
    // device_clients[0] = 0x01 (host/client 0 controls device 0)
    // device_clients[1] = 0x02 (client 1 controls device 1)
    uint32_t* device_clients = (uint32_t*)(sync_buf + 88);
    device_clients[0] = htonl(0x01);
    device_clients[1] = htonl(0x02);

    // nick[32] at offset 152: client's nickname (zero-padded)
    strncpy((char*)(sync_buf + 152), ctx->client_nick, RA_NICK_LEN - 1);

    if (!RA_sendCmd(fd, RA_CMD_SYNC, sync_buf, sizeof(sync_buf))) {
        LOG_info("RA server: failed to send SYNC\n");
        return -1;
    }
    LOG_info("RA server: sent SYNC (%zu bytes, frame=%u, client_num=%u)\n",
             sizeof(sync_buf), ctx->start_frame, ctx->client_num);

    //
    // Step 8: Receive CMD_PLAY from client
    //
    // Client may send other commands before PLAY, so loop.
    bool got_play = false;
    for (int attempts = 0; attempts < 50 && !got_play; attempts++) {
        RA_PacketHeader play_hdr;
        uint8_t play_buf[8];
        if (!RA_recvCmd(fd, &play_hdr, play_buf, sizeof(play_buf), 10000)) {
            LOG_info("RA server: timeout waiting for PLAY\n");
            return -1;
        }

        if (play_hdr.cmd == RA_CMD_PLAY) {
            got_play = true;
            LOG_info("RA server: received PLAY from client\n");
        }
        // Other commands (e.g. NICK re-sends) are consumed and ignored
    }

    if (!got_play) {
        LOG_info("RA server: never received PLAY\n");
        return -1;
    }

    //
    // Step 9: Send CMD_MODE (player assignment confirmation)
    //
    // CMD_MODE payload (60 bytes) - must match RA's struct mode_payload exactly:
    //   uint32_t frame                                  offset 0
    //   uint32_t mode_flags: (1<<31)=YOU | (1<<30)=PLAYING | client_num  offset 4
    //   uint32_t devices: bitmask of devices assigned   offset 8
    //   uint8_t  share_modes[16]                        offset 12
    //   char     nick[32]   (RA_NICK_LEN)              offset 28
    //
    // Total: 4 + 4 + 4 + 16 + 32 = 60 bytes (NETPLAY_NICK_LEN = 32 in RA)
    uint8_t mode_buf[60];
    memset(mode_buf, 0, sizeof(mode_buf));
    uint32_t* mode_words = (uint32_t*)mode_buf;

    mode_words[0] = htonl(ctx->start_frame);                        // frame
    mode_words[1] = htonl((1U << 31) | (1U << 30) | ctx->client_num); // YOU + PLAYING + client_num
    mode_words[2] = htonl(0x02);                                     // device 1 assigned (bitmask)

    // share_modes[16] at offset 12: all zeros
    // nick[32] at offset 28: client's nickname (zero-padded)
    strncpy((char*)(mode_buf + 28), ctx->client_nick, RA_NICK_LEN - 1);

    if (!RA_sendCmd(fd, RA_CMD_MODE, mode_buf, sizeof(mode_buf))) {
        LOG_info("RA server: failed to send MODE\n");
        return -1;
    }

    LOG_info("RA server: handshake complete (client '%s' assigned client_num=%u)\n",
             ctx->client_nick, ctx->client_num);

    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// LAN Discovery
//////////////////////////////////////////////////////////////////////////////

bool RA_sendDiscoveryQuery(int udp_fd) {
    if (udp_fd < 0) return false;

    // RA discovery query is just the 4-byte magic header
    uint32_t query = htonl(RA_DISCOVERY_QUERY_MAGIC);

    struct sockaddr_in bcast = {0};
    bcast.sin_family = AF_INET;
    bcast.sin_addr.s_addr = INADDR_BROADCAST;
    bcast.sin_port = htons(RA_DISCOVERY_PORT);

    ssize_t sent = sendto(udp_fd, &query, sizeof(query), 0,
                          (struct sockaddr*)&bcast, sizeof(bcast));
    return (sent == sizeof(query));
}

int RA_receiveDiscoveryResponses(int udp_fd, RA_DiscoveredHost* hosts,
                                  int* current_count, int max_hosts) {
    if (udp_fd < 0 || !hosts || !current_count) return 0;

    RA_DiscoveryPacket pkt;
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);

    while (recvfrom(udp_fd, &pkt, sizeof(pkt), MSG_DONTWAIT,
                    (struct sockaddr*)&sender, &sender_len) >= (ssize_t)sizeof(pkt)) {

        if (ntohl(pkt.header) != RA_DISCOVERY_RESPONSE_MAGIC) {
            sender_len = sizeof(sender);
            continue;
        }

        char ip[16];
        inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip));

        // Check for duplicates
        bool found = false;
        for (int i = 0; i < *current_count; i++) {
            if (strcmp(hosts[i].host_ip, ip) == 0) {
                found = true;
                break;
            }
        }

        if (!found && *current_count < max_hosts) {
            RA_DiscoveredHost* h = &hosts[*current_count];
            strncpy(h->host_ip, ip, sizeof(h->host_ip) - 1);
            h->host_ip[sizeof(h->host_ip) - 1] = '\0';

            // RA sends port in host byte order as int32
            h->port = (uint16_t)ntohl(pkt.port);
            h->content_crc = (uint32_t)ntohl(pkt.content_crc);

            strncpy(h->nick, pkt.nick, RA_NICK_LEN - 1);
            h->nick[RA_NICK_LEN - 1] = '\0';
            strncpy(h->core, pkt.core, RA_HOST_STR_LEN - 1);
            h->core[RA_HOST_STR_LEN - 1] = '\0';
            strncpy(h->core_version, pkt.core_version, RA_HOST_STR_LEN - 1);
            h->core_version[RA_HOST_STR_LEN - 1] = '\0';
            strncpy(h->content, pkt.content, RA_HOST_LONGSTR_LEN - 1);
            h->content[RA_HOST_LONGSTR_LEN - 1] = '\0';

            LOG_info("RA discovery: found host %s:%u nick=%s core=%s game=%s\n",
                     h->host_ip, h->port, h->nick, h->core, h->content);

            (*current_count)++;
        }

        sender_len = sizeof(sender);
    }

    return *current_count;
}
