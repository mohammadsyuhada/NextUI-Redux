#ifndef EMU_OVERLAY_SDL_H
#define EMU_OVERLAY_SDL_H

#include "emu_overlay_render.h"

// Get the SDL render backend.
// Before calling, set these environment variables:
//   EMU_OVERLAY_FONT — path to TTF font file
EmuOvlRenderBackend* overlay_sdl_get_backend(void);

#endif
