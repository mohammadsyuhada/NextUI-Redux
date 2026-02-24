#ifndef __FFPLAY_ENGINE_H__
#define __FFPLAY_ENGINE_H__

#include <stdbool.h>
#include "video_browser.h" // for MAX_SUBTITLE_FILES

// Forward declaration — full type comes from SDL via api.h in callers
struct SDL_Surface;

typedef enum {
	FFPLAY_SOURCE_LOCAL,
	FFPLAY_SOURCE_STREAM,
} FfplaySourceType;

typedef struct {
	char path[2048];		   // Video file path or stream URL (YouTube URLs can be very long)
	char subtitle_path[512];   // Subtitle source: external file or video path (empty = none)
	bool subtitle_is_external; // true = external .srt/.ass file, false = embedded in video
	char title[256];		   // Window title (empty = use filename/URL)
	char decryption_key[64];   // ClearKey hex string for DASH DRM (empty = none)
	int start_position_sec;	   // Seek position (0 = start)
	FfplaySourceType source;   // LOCAL or STREAM
	bool is_stream;			   // Stream mode flag

	// Multi-subtitle support: each entry becomes a separate -vf argument
	int subtitle_count;
	char subtitle_paths[MAX_SUBTITLE_FILES][512];
	char subtitle_labels[MAX_SUBTITLE_FILES][32];

	int screen_width; // Device screen width for resolution cap (0 = no cap)
	bool is_hevc;	  // true = HEVC/H.265 codec (enables aggressive decode opts)
} FfplayConfig;

// Play a video using ffplay subprocess
// This function releases PAD, forks ffplay, waits for it to exit, then re-initializes PAD.
// On TG5050, recovers the display pipeline after ffplay exits.
// Returns the ffplay exit code (0 = normal exit, non-zero = error)
int FfplayEngine_play(FfplayConfig* config);

// Stop the currently running ffplay process (if any)
void FfplayEngine_stop(void);

// TG5050: Release display before launching an external binary (ffplay, keyboard, etc.)
// to avoid DRM master conflicts. No-op on non-TG5050 platforms.
void FfplayEngine_prepareForExternal(void);

// TG5050: Restore display after external binary exits.
// No-op on non-TG5050 platforms or if prepareForExternal was not called.
void FfplayEngine_recoverDisplay(void);

// Get the new screen surface after TG5050 display recovery.
// Returns non-NULL if display was recovered (callers MUST update their screen pointer).
// Returns NULL if no recovery was needed.
struct SDL_Surface* FfplayEngine_getReinitScreen(void);

#endif
