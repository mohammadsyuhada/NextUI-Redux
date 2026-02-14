/*
 * NextUI Rollback Netplay Engine
 * Implements rollback/speculative execution for RetroArch host compatibility.
 *
 * When connected to an RA host, NextUI cannot use lockstep (RA never waits).
 * Instead we:
 *   1. Save state every frame to a ring buffer
 *   2. Predict remote input (copy last known) and run speculatively
 *   3. When real input arrives and differs from prediction, rewind & replay
 *   4. Exchange CRC per frame for desync detection
 *   5. Handle CMD_LOAD_SAVESTATE for desync recovery
 */

#ifndef NETPLAY_ROLLBACK_H
#define NETPLAY_ROLLBACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

// Ring buffer size must be power of 2
#define ROLLBACK_BUFFER_SIZE  64
#define ROLLBACK_BUFFER_MASK  (ROLLBACK_BUFFER_SIZE - 1)

// How many frames we can be ahead of the last confirmed remote input
// before we start worrying (RA typically allows ~10-15 frames ahead)
#define ROLLBACK_MAX_AHEAD    10

// CRC check interval (every N frames). 0 = every frame.
// Checking every frame is expensive; every 4 is a reasonable trade-off.
#define ROLLBACK_CRC_INTERVAL 4

//////////////////////////////////////////////////////////////////////////////
// Callback Types
//////////////////////////////////////////////////////////////////////////////

typedef size_t (*Rollback_SerializeSizeFn)(void);
typedef bool   (*Rollback_SerializeFn)(void* data, size_t size);
typedef bool   (*Rollback_UnserializeFn)(const void* data, size_t size);
typedef void   (*Rollback_CoreRunFn)(void);

//////////////////////////////////////////////////////////////////////////////
// Per-Frame Slot
//////////////////////////////////////////////////////////////////////////////

typedef struct {
    uint16_t local_input;
    uint16_t remote_input;
    bool     remote_confirmed;  // true = real input received, false = predicted
    uint32_t crc;               // CRC32 of state at this frame (0 = not computed)
    bool     state_saved;       // true if state_buffer has valid data for this frame
} RollbackFrameSlot;

//////////////////////////////////////////////////////////////////////////////
// Rollback State
//////////////////////////////////////////////////////////////////////////////

typedef struct {
    // Active flag
    bool active;

    // Network
    int      tcp_fd;
    uint32_t client_num;     // Our client number from RA handshake

    // Core callbacks
    Rollback_SerializeSizeFn serialize_size_fn;
    Rollback_SerializeFn     serialize_fn;
    Rollback_UnserializeFn   unserialize_fn;
    Rollback_CoreRunFn       core_run_fn;

    // State ring buffer
    void**   state_buffer;   // Array of ROLLBACK_BUFFER_SIZE state buffers
    size_t   state_size;     // Size of each serialized state

    // Frame tracking
    uint32_t self_frame;     // Next frame to execute (current frame)
    uint32_t read_frame;     // Latest frame with confirmed remote input
    uint32_t start_frame;    // Frame number at sync start (from RA host)

    // Input ring buffer
    RollbackFrameSlot frames[ROLLBACK_BUFFER_SIZE];

    // Replay state
    bool     replaying;      // true during replay (A/V should be suppressed)

    // Status
    bool     connected;
    bool     desync_detected;
    char     status_msg[128];

    // Thread safety
    pthread_mutex_t mutex;
} RollbackState;

//////////////////////////////////////////////////////////////////////////////
// Global rollback state (singleton, like the existing netplay state)
//////////////////////////////////////////////////////////////////////////////

/**
 * Initialize the rollback engine.
 * Call after RA handshake completes.
 *
 * @param tcp_fd         Connected TCP socket to RA host
 * @param client_num     Our client number from CMD_SYNC
 * @param start_frame    Server's frame count from CMD_SYNC
 * @param serialize_size Core's retro_serialize_size
 * @param serialize      Core's retro_serialize
 * @param unserialize    Core's retro_unserialize
 * @param core_run       Core's retro_run
 * @return 0 on success, -1 on failure (e.g., out of memory)
 */
int Rollback_init(int tcp_fd, uint32_t client_num, uint32_t start_frame,
                  Rollback_SerializeSizeFn serialize_size,
                  Rollback_SerializeFn serialize,
                  Rollback_UnserializeFn unserialize,
                  Rollback_CoreRunFn core_run);

/**
 * Shut down the rollback engine and free resources.
 */
void Rollback_quit(void);

/**
 * Per-frame update. Call this instead of Netplay_update when in rollback mode.
 *
 * This function:
 *   1. Saves current state to ring buffer
 *   2. Sends local input to RA host
 *   3. Receives available remote inputs (non-blocking)
 *   4. If any past prediction was wrong, rewinds and replays
 *   5. Predicts remote input for current frame if not yet received
 *   6. Advances frame counter
 *
 * @param local_input  Local player's input for this frame
 * @return 1 = run frame (always, rollback never stalls), 0 = error/disconnected
 */
int Rollback_update(uint16_t local_input);

/**
 * Get synchronized input for a player port.
 * Port 0 = host (player 1), Port 1 = us (player 2).
 * During normal play, returns confirmed or predicted input.
 * During replay, returns the corrected input for the replay frame.
 *
 * @param port  Player port (0 or 1)
 * @return Input state for the given port
 */
uint16_t Rollback_getInput(unsigned port);

/**
 * Called after core.run() completes for the current frame.
 * Advances the frame counter.
 */
void Rollback_postFrame(void);

/**
 * Check if currently replaying (for A/V suppression).
 * When true, audio and video callbacks should be suppressed.
 */
bool Rollback_isReplaying(void);

/**
 * Check if the rollback engine is active and connected.
 */
bool Rollback_isActive(void);

/**
 * Check if the rollback engine is connected (for Multiplayer_isActive).
 */
bool Rollback_isConnected(void);

/**
 * Get the current status message.
 */
const char* Rollback_getStatusMessage(void);

/**
 * Pause/resume support.
 */
void Rollback_pause(void);
void Rollback_resume(void);

/**
 * Disconnect from RA host.
 */
void Rollback_disconnect(void);

#endif /* NETPLAY_ROLLBACK_H */
