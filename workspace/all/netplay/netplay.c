/*
 * NextUI Netplay Facade
 * Dispatches between lockstep (NextUI-to-NextUI) and rollback (NextUI-to-RetroArch) modes.
 *
 * Protocol detection:
 * - NextUI host sends state data first (within ~16ms after connection)
 * - RA host waits for client header (never sends first)
 * - We peek with 500ms timeout to detect which protocol the host speaks
 */

#include "netplay.h"
#include "netplay_lockstep.h"
#include "netplay_rollback.h"
#include "ra_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef LOG_info
#define LOG_info(...) fprintf(stderr, __VA_ARGS__)
#endif

// Facade state (minimal - only tracks rollback mode and RA info)
static struct {
    bool rollback_mode;
    bool protocol_detected;
    Netplay_CoreRunFn core_run_fn;

    // Core info for RA handshake
    char ra_core_name[32];
    char ra_core_version[32];
    uint32_t ra_content_crc;
} facade = {0};

//////////////////////////////////////////////////////////////////////////////
// Protocol Detection
//////////////////////////////////////////////////////////////////////////////

// Detect whether the remote host speaks RA or NextUI protocol.
// Called once during the first state sync attempt for client mode.
//
// Returns true if RA protocol detected and rollback mode initialized.
// Returns false if NextUI protocol detected (caller proceeds with lockstep).
static bool detect_and_init_rollback(Netplay_SerializeSizeFn serialize_size_fn,
                                     Netplay_SerializeFn serialize_fn,
                                     Netplay_UnserializeFn unserialize_fn) {
    int tcp_fd = Lockstep_getTcpFd();
    if (tcp_fd < 0 || Lockstep_getMode() != NETPLAY_CLIENT) return false;

    // Peek at incoming data with 500ms timeout
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(tcp_fd, &fds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};  // 500ms

    int sel = select(tcp_fd + 1, &fds, NULL, NULL, &tv);

    if (sel > 0) {
        // Data available from host - NextUI host sending state
        LOG_info("Netplay: data received from host - using NextUI lockstep protocol\n");
        return false;
    }

    // No data within 500ms - likely RA host waiting for our header
    LOG_info("Netplay: no data from host in 500ms - attempting RA handshake\n");

    if (!facade.core_run_fn) {
        LOG_info("Netplay: core_run callback not set, cannot use rollback mode\n");
        return false;
    }

    // Attempt RA client handshake
    RA_HandshakeCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.tcp_fd = tcp_fd;
    ctx.content_crc = facade.ra_content_crc;
    strncpy(ctx.nick, "NextUI", RA_NICK_LEN - 1);
    strncpy(ctx.core_name, facade.ra_core_name, RA_CORE_NAME_LEN - 1);
    strncpy(ctx.core_version, facade.ra_core_version, RA_CORE_VERSION_LEN - 1);

    if (RA_clientHandshake(&ctx) != 0) {
        LOG_info("Netplay: RA handshake failed, disconnecting\n");
        return false;
    }

    LOG_info("Netplay: RA handshake success - initializing rollback engine\n");
    LOG_info("Netplay: RA server nick='%s', client_num=%u, start_frame=%u\n",
             ctx.server_nick, ctx.client_num, ctx.start_frame);

    // Initialize rollback engine (takes ownership of TCP fd)
    int result = Rollback_init(tcp_fd, ctx.client_num, ctx.start_frame,
                               false,  // is_server = false (we are client)
                               serialize_size_fn, serialize_fn, unserialize_fn,
                               facade.core_run_fn);

    if (result != 0) {
        LOG_info("Netplay: rollback init failed\n");
        return false;
    }

    // Detach fd from lockstep (rollback now owns it) and reset lockstep state
    Lockstep_detachTcpFd();

    facade.rollback_mode = true;
    facade.protocol_detected = true;

    LOG_info("Netplay: rollback mode active\n");
    return true;
}

// Detect whether an incoming client speaks RA or NextUI protocol.
// Called once when host accepts a connection (state=SYNCING).
//
// Returns true if RA protocol detected and rollback mode initialized.
// Returns false if NextUI protocol detected (caller proceeds with lockstep).
static bool detect_ra_client_and_init_rollback(Netplay_SerializeSizeFn serialize_size_fn,
                                                Netplay_SerializeFn serialize_fn,
                                                Netplay_UnserializeFn unserialize_fn) {
    int tcp_fd = Lockstep_getTcpFd();
    if (tcp_fd < 0 || Lockstep_getMode() != NETPLAY_HOST) return false;

    // Peek at incoming data with 500ms timeout
    // RA client sends its header first. NextUI client waits for state from host.
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(tcp_fd, &fds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};  // 500ms

    int sel = select(tcp_fd + 1, &fds, NULL, NULL, &tv);

    if (sel <= 0) {
        // No data within 500ms - NextUI client (waits for state from us)
        LOG_info("Netplay: no data from client in 500ms - using NextUI lockstep protocol\n");
        return false;
    }

    // Data available - peek at first 4 bytes to check for RANP magic
    uint32_t peek_magic = 0;
    ssize_t peeked = recv(tcp_fd, &peek_magic, sizeof(peek_magic), MSG_PEEK);
    if (peeked < (ssize_t)sizeof(peek_magic)) {
        LOG_info("Netplay: peek failed (%zd bytes), assuming NextUI client\n", peeked);
        return false;
    }

    if (ntohl(peek_magic) != RA_MAGIC) {
        // Not RA magic - NextUI client sending something else
        LOG_info("Netplay: client magic 0x%08x is not RANP - using NextUI lockstep\n",
                 ntohl(peek_magic));
        return false;
    }

    LOG_info("Netplay: RA client detected (RANP magic) - performing server handshake\n");

    if (!facade.core_run_fn) {
        LOG_info("Netplay: core_run callback not set, cannot use rollback mode\n");
        return false;
    }

    // Perform RA server handshake
    RA_ServerHandshakeCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.tcp_fd = tcp_fd;
    ctx.content_crc = facade.ra_content_crc;
    ctx.start_frame = 0;  // Start from frame 0
    strncpy(ctx.nick, "NextUI", RA_NICK_LEN - 1);
    strncpy(ctx.core_name, facade.ra_core_name, RA_CORE_NAME_LEN - 1);
    strncpy(ctx.core_version, facade.ra_core_version, RA_CORE_VERSION_LEN - 1);

    if (RA_serverHandshake(&ctx) != 0) {
        LOG_info("Netplay: RA server handshake failed, disconnecting\n");
        return false;
    }

    LOG_info("Netplay: RA server handshake success - initializing rollback engine\n");
    LOG_info("Netplay: RA client nick='%s', client_num=%u\n",
             ctx.client_nick, ctx.client_num);

    // NOTE: Initial savestate is NOT sent here. Sending CMD_LOAD_SAVESTATE
    // immediately after handshake crashes RA because RA hasn't finished its
    // post-handshake initialization (buffer allocation). Instead, the rollback
    // engine sends it on the first Rollback_update() call via force_send_savestate.

    // Initialize rollback engine in server mode
    // Server is client_num 0, remote client is client_num 1
    int result = Rollback_init(tcp_fd, 0, ctx.start_frame,
                               true,  // is_server = true
                               serialize_size_fn, serialize_fn, unserialize_fn,
                               facade.core_run_fn);

    if (result != 0) {
        LOG_info("Netplay: rollback init failed\n");
        return false;
    }

    // Detach fd from lockstep (rollback now owns it)
    Lockstep_detachTcpFd();

    facade.rollback_mode = true;
    facade.protocol_detected = true;

    LOG_info("Netplay: server rollback mode active\n");
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Initialization
//////////////////////////////////////////////////////////////////////////////

void Netplay_init(void) {
    // Lockstep_init() has its own guard (ls.initialized), so it's safe
    // to call multiple times. Don't clear facade state here - core_run_fn
    // and core info may have been set before this is called.
    Lockstep_init();
}

void Netplay_quit(void) {
    // Always call Rollback_quit() to free ring buffer, even if rollback_mode
    // was already cleared by a prior Netplay_disconnect(). Rollback_quit()
    // is safe to call when rollback was never initialized.
    Rollback_quit();
    facade.rollback_mode = false;
    Lockstep_quit();
    memset(&facade, 0, sizeof(facade));
}

bool Netplay_checkCoreSupport(const char* core_name) {
    return Lockstep_checkCoreSupport(core_name);
}

//////////////////////////////////////////////////////////////////////////////
// Connection Management
//////////////////////////////////////////////////////////////////////////////

int Netplay_startHost(const char* game_name, uint32_t game_crc, const char* hotspot_ip) {
    int result = Lockstep_startHost(game_name, game_crc, hotspot_ip);
    if (result == 0) {
        // Use game_crc from caller (calculated from ROM data), not facade.ra_content_crc
        // which may still be 0 from early Netplay_setCoreInfo() call.
        facade.ra_content_crc = game_crc;
        // Set RA discovery info AFTER Lockstep_startHost, because startHost calls
        // Lockstep_init() which memsets the state (clearing any prior setRACoreInfo data).
        Lockstep_setRACoreInfo(facade.ra_core_name, facade.ra_core_version,
                               game_name, game_crc);
    }
    return result;
}

int Netplay_stopHost(void) {
    return Lockstep_stopHost();
}

int Netplay_stopHostFast(void) {
    return Lockstep_stopHostFast();
}

void Netplay_stopBroadcast(void) {
    Lockstep_stopBroadcast();
}

int Netplay_connectToHost(const char* ip, uint16_t port) {
    facade.protocol_detected = false;
    return Lockstep_connectToHost(ip, port);
}

void Netplay_disconnect(void) {
    if (facade.rollback_mode) {
        Rollback_disconnect();
        facade.rollback_mode = false;
        facade.protocol_detected = false;
        return;
    }
    Lockstep_disconnect();
}

//////////////////////////////////////////////////////////////////////////////
// Status Queries
//////////////////////////////////////////////////////////////////////////////

NetplayMode Netplay_getMode(void) {
    if (facade.rollback_mode) {
        return Rollback_isServer() ? NETPLAY_HOST : NETPLAY_CLIENT;
    }
    return Lockstep_getMode();
}

NetplayState Netplay_getState(void) {
    if (facade.rollback_mode) {
        return Rollback_isConnected() ? NETPLAY_STATE_PLAYING : NETPLAY_STATE_DISCONNECTED;
    }
    return Lockstep_getState();
}

bool Netplay_isConnected(void) {
    if (facade.rollback_mode) return Rollback_isConnected();
    return Lockstep_isConnected();
}

bool Netplay_isActive(void) {
    if (facade.rollback_mode) return Rollback_isActive();
    return Lockstep_isActive();
}

bool Netplay_isUsingHotspot(void) {
    return Lockstep_isUsingHotspot();
}

const char* Netplay_getStatusMessage(void) {
    if (facade.rollback_mode) return Rollback_getStatusMessage();
    return Lockstep_getStatusMessage();
}

const char* Netplay_getLocalIP(void) {
    return Lockstep_getLocalIP();
}

bool Netplay_hasNetworkConnection(void) {
    return Lockstep_hasNetworkConnection();
}

//////////////////////////////////////////////////////////////////////////////
// Discovery
//////////////////////////////////////////////////////////////////////////////

int Netplay_startDiscovery(void) {
    return Lockstep_startDiscovery();
}

void Netplay_stopDiscovery(void) {
    Lockstep_stopDiscovery();
}

int Netplay_getDiscoveredHosts(NetplayHostInfo* hosts, int max_hosts) {
    return Lockstep_getDiscoveredHosts(hosts, max_hosts);
}

//////////////////////////////////////////////////////////////////////////////
// Frame Synchronization
//////////////////////////////////////////////////////////////////////////////

bool Netplay_preFrame(void) {
    if (facade.rollback_mode) return true;  // Rollback never stalls
    return Lockstep_preFrame();
}

uint16_t Netplay_getInputState(unsigned port) {
    if (facade.rollback_mode) return Rollback_getInput(port);
    return Lockstep_getInputState(port);
}

uint32_t Netplay_getPlayerButtons(unsigned port, uint32_t local_buttons) {
    if (facade.rollback_mode && Rollback_isConnected()) {
        return Rollback_getInput(port);
    }
    return Lockstep_getPlayerButtons(port, local_buttons);
}

void Netplay_setLocalInput(uint16_t input) {
    if (facade.rollback_mode) return;  // Rollback gets input via Rollback_update()
    Lockstep_setLocalInput(input);
}

void Netplay_postFrame(void) {
    if (facade.rollback_mode) {
        Rollback_postFrame();
        return;
    }
    Lockstep_postFrame();
}

bool Netplay_shouldStall(void) {
    if (facade.rollback_mode) return false;  // Rollback never stalls
    return Lockstep_shouldStall();
}

bool Netplay_shouldSilenceAudio(void) {
    if (facade.rollback_mode) return Rollback_isReplaying();
    return Lockstep_shouldSilenceAudio();
}

//////////////////////////////////////////////////////////////////////////////
// State Synchronization
//////////////////////////////////////////////////////////////////////////////

int Netplay_sendState(const void* data, size_t size) {
    return Lockstep_sendState(data, size);
}

int Netplay_receiveState(void* data, size_t size) {
    return Lockstep_receiveState(data, size);
}

bool Netplay_needsStateSync(void) {
    return Lockstep_needsStateSync();
}

void Netplay_completeStateSync(void) {
    Lockstep_completeStateSync();
}

//////////////////////////////////////////////////////////////////////////////
// Pause/Resume
//////////////////////////////////////////////////////////////////////////////

void Netplay_pause(void) {
    if (facade.rollback_mode) {
        Rollback_pause();
        return;
    }
    Lockstep_pause();
}

void Netplay_resume(void) {
    if (facade.rollback_mode) {
        Rollback_resume();
        return;
    }
    Lockstep_resume();
}

void Netplay_pollWhilePaused(void) {
    if (facade.rollback_mode) {
        Rollback_pollWhilePaused();
        return;
    }
    Lockstep_pollWhilePaused();
}

bool Netplay_isPaused(void) {
    if (facade.rollback_mode) return Rollback_isPaused();
    return Lockstep_isPaused();
}

//////////////////////////////////////////////////////////////////////////////
// Main Loop Update
//////////////////////////////////////////////////////////////////////////////

int Netplay_update(uint16_t local_input,
                   Netplay_SerializeSizeFn serialize_size_fn,
                   Netplay_SerializeFn serialize_fn,
                   Netplay_UnserializeFn unserialize_fn) {

    // If already in rollback mode, dispatch to rollback engine
    if (facade.rollback_mode) {
        if (!Rollback_isConnected()) {
            Netplay_disconnect();
            return 1;  // Run frame normally after disconnect
        }
        return Rollback_update(local_input);
    }

    // Protocol detection (before lockstep state sync)
    if (!facade.protocol_detected && Lockstep_needsStateSync()) {
        if (serialize_size_fn && serialize_fn && unserialize_fn) {
            if (Lockstep_getMode() == NETPLAY_CLIENT) {
                // Client mode: detect if host is RA or NextUI
                if (detect_and_init_rollback(serialize_size_fn, serialize_fn, unserialize_fn)) {
                    return 0;  // Skip this frame (rollback init completed)
                }
            } else if (Lockstep_getMode() == NETPLAY_HOST) {
                // Host mode: detect if client is RA or NextUI
                if (detect_ra_client_and_init_rollback(serialize_size_fn, serialize_fn, unserialize_fn)) {
                    return 0;  // Skip this frame (rollback init completed)
                }
            }
        }
        // Not RA - proceed with normal lockstep
        facade.protocol_detected = true;
    }

    // Dispatch to lockstep engine
    return Lockstep_update(local_input, serialize_size_fn, serialize_fn, unserialize_fn);
}

//////////////////////////////////////////////////////////////////////////////
// Rollback Support
//////////////////////////////////////////////////////////////////////////////

void Netplay_setCoreRunCallback(Netplay_CoreRunFn core_run_fn) {
    facade.core_run_fn = core_run_fn;
}

void Netplay_setCoreInfo(const char* core_name, const char* core_version, uint32_t content_crc) {
    if (core_name) strncpy(facade.ra_core_name, core_name, sizeof(facade.ra_core_name) - 1);
    if (core_version) strncpy(facade.ra_core_version, core_version, sizeof(facade.ra_core_version) - 1);
    facade.ra_content_crc = content_crc;

    // Also propagate to lockstep for RA discovery responses when hosting
    Lockstep_setRACoreInfo(core_name, core_version, NULL, content_crc);
}

bool Netplay_isRollbackReplaying(void) {
    return facade.rollback_mode && Rollback_isReplaying();
}

bool Netplay_isRollbackMode(void) {
    return facade.rollback_mode;
}
