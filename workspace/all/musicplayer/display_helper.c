#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "api.h"
#include "display_helper.h"

// New screen surface after TG5050 display recovery (NULL = no recovery needed)
static SDL_Surface* reinit_screen = NULL;

// Whether display has been released for an external binary
static bool display_released = false;

// Defined in generic_video.c — only the video pipeline, no fonts/config/assets.
extern void PLAT_quitVideo(void);
extern SDL_Surface* PLAT_initVideo(void);

void DisplayHelper_prepareForExternal(void) {
	if (strcmp(PLATFORM, "tg5050") != 0)
		return;

	// Keep SDL alive during video subsystem teardown.
	// PLAT_quitVideo calls SDL_QuitSubSystem(SDL_INIT_VIDEO) — if no other
	// subsystem is alive, SDL would fully quit and lose all state.
	SDL_InitSubSystem(SDL_INIT_EVENTS);

	PLAT_quitVideo();
	display_released = true;
}

void DisplayHelper_recoverDisplay(void) {
	if (!display_released)
		return;

	reinit_screen = PLAT_initVideo();

	SDL_QuitSubSystem(SDL_INIT_EVENTS);
	display_released = false;
}

SDL_Surface* DisplayHelper_getReinitScreen(void) {
	return reinit_screen;
}
