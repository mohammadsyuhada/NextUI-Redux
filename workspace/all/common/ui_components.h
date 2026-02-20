#ifndef UI_COMPONENTS_H
#define UI_COMPONENTS_H

#include "sdl.h"

void UI_renderConfirmDialog(SDL_Surface* dst, const char* title,
							const char* subtitle);

void UI_calcImageFit(int img_w, int img_h, int max_w, int max_h,
					 int* out_w, int* out_h);

SDL_Surface* UI_convertSurface(SDL_Surface* surface, SDL_Surface* screen);

void UI_renderCenteredMessage(SDL_Surface* dst, const char* message);

// Render the top menu bar: semi-transparent background, title text (left), hardware group (right).
// Returns the width of the hardware group (ow) for callers that need it.
int UI_renderButtonHintBar(SDL_Surface* dst, char** right_pairs, char** left_pairs);

int UI_renderMenuBar(SDL_Surface* screen, const char* title, int show_setting);

// Render a splash screen with a title and "Loading..." subtitle, then flip.
// Call immediately after GFX_init() for instant visual feedback during app startup.
void UI_showSplashScreen(SDL_Surface* screen, const char* title);

// Render a full-screen semi-transparent overlay with title/subtitle text.
// Used for blocking operations (e.g. WiFi/BT toggle) with a cancel hint.
void UI_renderLoadingOverlay(SDL_Surface* dst, const char* title, const char* subtitle);

#endif // UI_COMPONENTS_H
