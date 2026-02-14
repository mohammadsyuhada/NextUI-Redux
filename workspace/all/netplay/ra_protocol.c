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

bool RA_sendInput(int fd, uint32_t frame_num, uint32_t client_num, uint16_t input) {
    // RA protocol v6 CMD_INPUT payload:
    //   uint32_t frame_num
    //   uint32_t is_server_input (0 for client)
    //   uint32_t player_num
    //   uint32_t input_size (number of uint32 words, 1 for JOYPAD)
    //   uint32_t input_data
    uint32_t payload[5];
    payload[0] = htonl(frame_num);
    payload[1] = htonl(0);            // Not server input
    payload[2] = htonl(client_num);
    payload[3] = htonl(1);            // 1 word of input
    payload[4] = htonl((uint32_t)input);

    return RA_sendCmd(fd, RA_CMD_INPUT, payload, sizeof(payload));
}

bool RA_parseInput(const void* data, uint32_t size,
                   uint32_t* frame_out, uint32_t* player_out, uint16_t* input_out) {
    if (size < 16) return false;  // Minimum: frame + is_server + player + input_size

    const uint32_t* p = (const uint32_t*)data;
    *frame_out  = ntohl(p[0]);
    // p[1] = is_server_input (we don't need this)
    *player_out = ntohl(p[2]);

    uint32_t input_size = ntohl(p[3]);
    if (input_size < 1 || size < 16 + input_size * 4) return false;

    *input_out = (uint16_t)ntohl(p[4]);
    return true;
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
    // Step 4: Send CMD_INFO
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
    // Step 5: Receive CMD_SYNC
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

    // The rest of CMD_SYNC contains device configs and possibly SRAM data.
    // For input-sync netplay with RETRO_DEVICE_JOYPAD we don't need to parse
    // device configs in detail - both sides just exchange joypad input.

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
