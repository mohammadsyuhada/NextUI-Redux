#include "ui_toast.h"
#include "api.h"
#include "defines.h"

// Toast is rendered to GPU layer 5 (highest) to appear above all content
#define LAYER_TOAST 5

void UI_renderToast(SDL_Surface* screen, const char* message, uint32_t toast_time) {
	if (!message || message[0] == '\0') {
		PLAT_clearLayers(LAYER_TOAST);
		return;
	}

	uint32_t now = SDL_GetTicks();
	if (now - toast_time >= TOAST_DURATION) {
		PLAT_clearLayers(LAYER_TOAST);
		return;
	}

	int hw = screen->w;
	int hh = screen->h;

	SDL_Surface* toast_text = TTF_RenderUTF8_Blended(font.medium, message, COLOR_WHITE);
	if (toast_text) {
		int border = SCALE1(2);
		int toast_w = toast_text->w + SCALE1(PADDING * 3);
		int toast_h = toast_text->h + SCALE1(12);
		int toast_x = (hw - toast_w) / 2;
		int toast_y = hh - SCALE1(BUTTON_SIZE + BUTTON_MARGIN + PADDING * 3) - toast_h;

		// Total surface size including border
		int surface_w = toast_w + border * 2;
		int surface_h = toast_h + border * 2;

		// Create surface for GPU layer rendering
		SDL_Surface* toast_surface = SDL_CreateRGBSurfaceWithFormat(0,
																	surface_w, surface_h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (toast_surface) {
			// Disable blending so fills are opaque
			SDL_SetSurfaceBlendMode(toast_surface, SDL_BLENDMODE_NONE);

			// Draw light gray border (outer rect)
			SDL_FillRect(toast_surface, NULL, SDL_MapRGBA(toast_surface->format, 200, 200, 200, 255));

			// Draw dark grey background (inner rect)
			SDL_Rect bg_rect = {border, border, toast_w, toast_h};
			SDL_FillRect(toast_surface, &bg_rect, SDL_MapRGBA(toast_surface->format, 40, 40, 40, 255));

			// Draw text centered within the toast (blend text onto surface)
			SDL_SetSurfaceBlendMode(toast_surface, SDL_BLENDMODE_BLEND);
			int text_x = border + (toast_w - toast_text->w) / 2;
			int text_y = border + (toast_h - toast_text->h) / 2;
			SDL_BlitSurface(toast_text, NULL, toast_surface, &(SDL_Rect){text_x, text_y});

			// Render to GPU layer at the correct screen position
			PLAT_clearLayers(LAYER_TOAST);
			PLAT_drawOnLayer(toast_surface, toast_x - border, toast_y - border,
							 surface_w, surface_h, 1.0f, false, LAYER_TOAST);

			SDL_FreeSurface(toast_surface);
		}
		SDL_FreeSurface(toast_text);
	}
}

void UI_clearToast(void) {
	PLAT_clearLayers(LAYER_TOAST);
}
