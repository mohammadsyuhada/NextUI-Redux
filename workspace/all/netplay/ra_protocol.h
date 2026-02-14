/*
 * RetroArch Netplay Protocol Module
 * Implements the RetroArch netplay wire protocol for cross-platform compatibility.
 *
 * When NextUI connects to a RetroArch host, it must speak RA's protocol:
 * - 24-byte connection header exchange
 * - CMD_NICK / CMD_INFO / CMD_SYNC handshake
 * - CMD_INPUT for per-frame input exchange
 * - CMD_CRC for per-frame state verification
 * - CMD_LOAD_SAVESTATE for desync recovery
 *
 * Reference: RetroArch network/netplay/netplay_private.h
 */

#ifndef RA_PROTOCOL_H
#define RA_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

//////////////////////////////////////////////////////////////////////////////
// Protocol Constants
//////////////////////////////////////////////////////////////////////////////

#define RA_MAGIC               0x52414E50  // "RANP" in big-endian
#define RA_PLATFORM_MAGIC      0x4E585549  // "NXUI" - NextUI platform identifier
#define RA_IMPL_MAGIC          0x4E585242  // "NXRB" - NextUI rollback impl

// Protocol version range we support
#define RA_PROTOCOL_VERSION_MIN  6
#define RA_PROTOCOL_VERSION_MAX  6
#define RA_PROTOCOL_VERSION      6

// Nickname max length
#define RA_NICK_LEN  32

// Core name/version max length (for CMD_INFO)
#define RA_CORE_NAME_LEN     32
#define RA_CORE_VERSION_LEN  32

//////////////////////////////////////////////////////////////////////////////
// RA Command IDs
//////////////////////////////////////////////////////////////////////////////

#define RA_CMD_ACK              0x0000
#define RA_CMD_NAK              0x0001
#define RA_CMD_DISCONNECT       0x0002
#define RA_CMD_INPUT            0x0003
#define RA_CMD_NOINPUT          0x0004
#define RA_CMD_NICK             0x0020
#define RA_CMD_PASSWORD         0x0021
#define RA_CMD_INFO             0x0022
#define RA_CMD_SYNC             0x0023
#define RA_CMD_SPECTATE         0x0024
#define RA_CMD_PLAY             0x0041
#define RA_CMD_MODE             0x0042
#define RA_CMD_CRC              0x0040
#define RA_CMD_REQUEST_SAVESTATE 0x0043
#define RA_CMD_LOAD_SAVESTATE   0x0044
#define RA_CMD_PAUSE            0x0045
#define RA_CMD_RESUME           0x0046
#define RA_CMD_CFG              0x0061
#define RA_CMD_CFG_ACK          0x0062

// Number of words of config state per client in CMD_SYNC
#define RA_NUM_CLIENTS          32
#define RA_MAX_DEVICES          16

//////////////////////////////////////////////////////////////////////////////
// Wire Structures
//////////////////////////////////////////////////////////////////////////////

// RA packet header (8 bytes) - prepended to every command
typedef struct __attribute__((packed)) {
    uint32_t cmd;     // Command ID (network byte order)
    uint32_t size;    // Payload size in bytes (network byte order)
} RA_PacketHeader;

// RA connection header (24 bytes) - exchanged during initial handshake
// Client version:
typedef struct __attribute__((packed)) {
    uint32_t magic;           // RA_MAGIC
    uint32_t platform_magic;  // Platform identifier
    uint32_t compression;     // 0 = no compression
    uint32_t proto_hi;        // Highest protocol version we support
    uint32_t proto_lo;        // Lowest protocol version we support
    uint32_t impl_magic;      // Implementation identifier
} RA_ClientHeader;

// Server version (same 24 bytes, different field meanings):
typedef struct __attribute__((packed)) {
    uint32_t magic;           // RA_MAGIC
    uint32_t platform_magic;  // Platform identifier
    uint32_t compression;     // 0 = no compression
    uint32_t salt;            // Password salt (0 if no password)
    uint32_t proto;           // Negotiated protocol version
    uint32_t impl_magic;      // Implementation identifier
} RA_ServerHeader;

// CMD_INFO payload (sent by client after NICK exchange)
typedef struct __attribute__((packed)) {
    uint32_t content_crc;                   // CRC32 of loaded content
    char     core_name[RA_CORE_NAME_LEN];   // Core library name
    char     core_version[RA_CORE_VERSION_LEN]; // Core library version
} RA_InfoPayload;

// CMD_SYNC payload (sent by server after receiving INFO)
// Variable-length: header + per-client device config + SRAM
// We parse it field by field rather than mapping a struct
// Fields (in order):
//   uint32_t frame_num          - Server's current frame
//   uint32_t connections        - Bitmask of connected clients
//   uint32_t client_num         - Our assigned client number
//   uint32_t share_modes[16]    - Device share modes
//   For each of 32 clients:
//     uint8_t  connected
//     uint8_t  paused
//     uint16_t reserved
//     uint32_t devices[16]      - Device-to-player mapping

// CMD_INPUT payload
// For protocol version 6:
//   uint32_t frame_num       - Frame this input is for
//   uint32_t is_server_input - 1 if this is the server's own input
//   uint32_t player_num      - Which player slot
//   uint32_t input_size      - Number of uint32 words of input
//   uint32_t input[]         - Input data (RETRO_DEVICE_JOYPAD = 1 word)

// CMD_CRC payload
typedef struct __attribute__((packed)) {
    uint32_t frame_num;   // Frame number (network byte order)
    uint32_t crc;         // CRC32 of serialized state (network byte order)
} RA_CRCPayload;

//////////////////////////////////////////////////////////////////////////////
// Handshake Context
//////////////////////////////////////////////////////////////////////////////

typedef struct {
    int      tcp_fd;
    uint32_t negotiated_proto;    // Protocol version negotiated with server
    uint32_t client_num;          // Our assigned client number (from CMD_SYNC)
    uint32_t start_frame;         // Server's frame count at sync time
    uint32_t content_crc;         // CRC32 of loaded content
    char     nick[RA_NICK_LEN];   // Our nickname
    char     core_name[RA_CORE_NAME_LEN];
    char     core_version[RA_CORE_VERSION_LEN];
    char     server_nick[RA_NICK_LEN]; // Server's nickname
} RA_HandshakeCtx;

//////////////////////////////////////////////////////////////////////////////
// Protocol Functions
//////////////////////////////////////////////////////////////////////////////

/**
 * Perform the client-side handshake with an RA host.
 * Must be called after TCP connection is established.
 *
 * Sequence:
 *   1. Send client connection header (24 bytes)
 *   2. Receive server connection header (24 bytes)
 *   3. Exchange CMD_NICK (both directions)
 *   4. Send CMD_INFO (content CRC, core name/version)
 *   5. Receive CMD_SYNC (frame count, client number, device config)
 *
 * @param ctx  Handshake context (tcp_fd, content_crc, nick, core_name, core_version must be set)
 * @return 0 on success, -1 on failure
 */
int RA_clientHandshake(RA_HandshakeCtx* ctx);

/**
 * Send an RA command with optional payload.
 * @param fd    TCP socket
 * @param cmd   Command ID (host byte order, will be converted)
 * @param data  Payload data (can be NULL if size == 0)
 * @param size  Payload size in bytes
 * @return true on success
 */
bool RA_sendCmd(int fd, uint32_t cmd, const void* data, uint32_t size);

/**
 * Receive an RA command with optional payload.
 * @param fd        TCP socket
 * @param hdr       Output: received header (cmd and size in host byte order)
 * @param data      Output: payload buffer (can be NULL)
 * @param max_size  Maximum bytes to read into data
 * @param timeout_ms  Timeout in milliseconds (0 = non-blocking)
 * @return true if a command was received
 */
bool RA_recvCmd(int fd, RA_PacketHeader* hdr, void* data, uint32_t max_size, int timeout_ms);

/**
 * Send input for a frame in RA format.
 * @param fd          TCP socket
 * @param frame_num   Frame number
 * @param client_num  Our client number
 * @param input       RETRO_DEVICE_JOYPAD input state (16-bit)
 * @return true on success
 */
bool RA_sendInput(int fd, uint32_t frame_num, uint32_t client_num, uint16_t input);

/**
 * Parse an RA CMD_INPUT payload.
 * @param data        Payload data
 * @param size        Payload size
 * @param frame_out   Output: frame number
 * @param player_out  Output: player number (0-based)
 * @param input_out   Output: input state (16-bit joypad)
 * @return true if parsed successfully
 */
bool RA_parseInput(const void* data, uint32_t size,
                   uint32_t* frame_out, uint32_t* player_out, uint16_t* input_out);

/**
 * Send CRC for a frame.
 * @param fd         TCP socket
 * @param frame_num  Frame number
 * @param crc        CRC32 of serialized state
 * @return true on success
 */
bool RA_sendCRC(int fd, uint32_t frame_num, uint32_t crc);

/**
 * Drain and discard remaining payload bytes if we didn't read the full payload.
 * @param fd    TCP socket
 * @param remaining  Number of bytes to drain
 * @return true on success
 */
bool RA_drainBytes(int fd, uint32_t remaining);

#endif /* RA_PROTOCOL_H */
