#ifndef UI_TOAST_H
#define UI_TOAST_H

#include <stdint.h>
#include "sdl.h"

#define TOAST_DURATION 3000

// Render toast notification to GPU layer (above all other content including scroll text).
// Call this at the end of your render function. Toast auto-hides after TOAST_DURATION.
void UI_renderToast(SDL_Surface* screen, const char* message, uint32_t toast_time);

// Clear toast from GPU layer (call when leaving screen or clearing state).
void UI_clearToast(void);

#endif // UI_TOAST_H
