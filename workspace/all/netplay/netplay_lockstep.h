/*
 * NextUI Lockstep Netplay Module
 * Pure lockstep synchronization: both devices must have same inputs before advancing.
 *
 * This module handles all NextUI-to-NextUI netplay:
 * - Host/client connection management
 * - Frame-synchronized input exchange
 * - State synchronization
 * - Discovery broadcasting/listening
 * - Pause/resume for menu
 *
 * Used by the netplay facade (netplay.c) for NextUI protocol connections.
 */

#ifndef NETPLAY_LOCKSTEP_H
#define NETPLAY_LOCKSTEP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "netplay.h"  // For NetplayMode, NetplayState, NetplayHostInfo, typedefs

// Initialize/cleanup
void Lockstep_init(void);
void Lockstep_quit(void);

// Core support check
bool Lockstep_checkCoreSupport(const char* core_name);

// Host mode
int  Lockstep_startHost(const char* game_name, uint32_t game_crc, const char* hotspot_ip);
int  Lockstep_stopHost(void);
int  Lockstep_stopHostFast(void);
void Lockstep_stopBroadcast(void);

// Client mode
int  Lockstep_connectToHost(const char* ip, uint16_t port);
void Lockstep_disconnect(void);

// Status queries
NetplayMode  Lockstep_getMode(void);
NetplayState Lockstep_getState(void);
bool Lockstep_isConnected(void);
bool Lockstep_isActive(void);
bool Lockstep_isUsingHotspot(void);
const char*  Lockstep_getStatusMessage(void);
const char*  Lockstep_getLocalIP(void);
bool Lockstep_hasNetworkConnection(void);

// Host discovery
int  Lockstep_startDiscovery(void);
void Lockstep_stopDiscovery(void);
int  Lockstep_getDiscoveredHosts(NetplayHostInfo* hosts, int max_hosts);

// Frame synchronization
bool     Lockstep_preFrame(void);
uint16_t Lockstep_getInputState(unsigned port);
uint32_t Lockstep_getPlayerButtons(unsigned port, uint32_t local_buttons);
void     Lockstep_setLocalInput(uint16_t input);
void     Lockstep_postFrame(void);
bool     Lockstep_shouldStall(void);
bool     Lockstep_shouldSilenceAudio(void);

// State synchronization
int  Lockstep_sendState(const void* data, size_t size);
int  Lockstep_receiveState(void* data, size_t size);
bool Lockstep_needsStateSync(void);
void Lockstep_completeStateSync(void);

// Pause/resume
void Lockstep_pause(void);
void Lockstep_resume(void);
void Lockstep_pollWhilePaused(void);
bool Lockstep_isPaused(void);

// Main loop update (lockstep only, no protocol detection)
// Returns: 1 = run frame, 0 = skip frame (stalled/syncing)
int Lockstep_update(uint16_t local_input,
                    Netplay_SerializeSizeFn serialize_size_fn,
                    Netplay_SerializeFn serialize_fn,
                    Netplay_UnserializeFn unserialize_fn);

// Set RA core info (for discovery response and server handshake)
void Lockstep_setRACoreInfo(const char* core_name, const char* core_version,
                            const char* content_name, uint32_t content_crc);

// Access TCP fd (for protocol detection by facade)
int Lockstep_getTcpFd(void);

// Detach TCP fd from lockstep (for handoff to rollback engine)
// Returns the fd and resets lockstep connection state without closing it
int Lockstep_detachTcpFd(void);

#endif /* NETPLAY_LOCKSTEP_H */
