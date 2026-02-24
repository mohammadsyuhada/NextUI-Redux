#ifndef __DISPLAY_HELPER_H__
#define __DISPLAY_HELPER_H__

// Forward declaration — full type comes from SDL via api.h in callers
struct SDL_Surface;

// TG5050: Release display before launching an external binary (keyboard, etc.)
// to avoid DRM master conflicts. No-op on non-TG5050 platforms.
void DisplayHelper_prepareForExternal(void);

// TG5050: Restore display after external binary exits.
// No-op on non-TG5050 platforms or if prepareForExternal was not called.
void DisplayHelper_recoverDisplay(void);

// Get the new screen surface after TG5050 display recovery.
// Returns non-NULL if display was recovered (callers MUST update their screen pointer).
// Returns NULL if no recovery was needed.
struct SDL_Surface* DisplayHelper_getReinitScreen(void);

#endif
