#include "ui_components.h"
#include "api.h"
#include "defines.h"

void UI_renderConfirmDialog(SDL_Surface* dst, const char* title,
							const char* subtitle) {
	int padding_x = SCALE1(PADDING * 4);
	int content_w = dst->w - padding_x * 2;

	SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format, 0, 0, 0));

	int btn_sz = SCALE1(BUTTON_SIZE);
	int btn_gap = SCALE1(BUTTON_TEXT_GAP);
	int btn_margin = SCALE1(BUTTON_MARGIN);

	int title_h = TTF_FontHeight(font.large);
	int total_h = title_h;
	if (subtitle)
		total_h += SCALE1(BUTTON_MARGIN) + TTF_FontHeight(font.small);
	total_h += SCALE1(BUTTON_MARGIN) + btn_sz;

	int y = (dst->h - total_h) / 2;

	// Title
	SDL_Rect title_rect = {padding_x, y, content_w, title_h};
	GFX_blitMessage(font.large, (char*)title, dst, &title_rect);

	// Subtitle (optional)
	if (subtitle) {
		int sub_h = TTF_FontHeight(font.small);
		y += title_h + SCALE1(BUTTON_MARGIN);
		SDL_Rect sub_rect = {padding_x, y, content_w, sub_h};
		GFX_blitMessage(font.small, (char*)subtitle, dst, &sub_rect);
		y += sub_h;
	} else {
		y += title_h;
	}

	// Buttons
	y += SCALE1(BUTTON_MARGIN);

	int cancel_w, confirm_w, th;
	TTF_SizeUTF8(font.tiny, "CANCEL", &cancel_w, &th);
	TTF_SizeUTF8(font.tiny, "CONFIRM", &confirm_w, &th);

	int btn1_w = btn_sz + btn_gap + cancel_w;
	int btn2_w = btn_sz + btn_gap + confirm_w;
	int total_btn_w = btn1_w + btn_margin + btn2_w;

	int bx = (dst->w - total_btn_w) / 2;
	GFX_blitButton("CANCEL", "B", dst, &(SDL_Rect){bx, y, 0, 0});
	bx += btn1_w + btn_margin;
	GFX_blitButton("CONFIRM", "A", dst, &(SDL_Rect){bx, y, 0, 0});
}

void UI_calcImageFit(int img_w, int img_h, int max_w, int max_h,
					 int* out_w, int* out_h) {
	double aspect_ratio = (double)img_h / img_w;
	int new_w = max_w;
	int new_h = (int)(new_w * aspect_ratio);

	if (new_h > max_h) {
		new_h = max_h;
		new_w = (int)(new_h / aspect_ratio);
	}

	*out_w = new_w;
	*out_h = new_h;
}

SDL_Surface* UI_convertSurface(SDL_Surface* surface, SDL_Surface* screen) {
	SDL_Surface* converted =
		SDL_ConvertSurfaceFormat(surface, screen->format->format, 0);
	if (converted) {
		SDL_FreeSurface(surface);
		return converted;
	}
	return surface;
}

void UI_renderCenteredMessage(SDL_Surface* dst, const char* message) {
	SDL_Rect fullscreen_rect = {0, 0, dst->w, dst->h};
	GFX_blitMessage(font.large, (char*)message, dst, &fullscreen_rect);
}

int UI_renderButtonHintBar(SDL_Surface* dst, char** right_pairs, char** left_pairs) {
	struct Hint {
		char* hint;
		char* button;
		int ow;
	};

	struct Hint hints[8];
	int count = 0;
	int total_w = 0;

	// Parse left pairs then right pairs sequentially
	char** groups[] = {left_pairs, right_pairs};
	for (int g = 0; g < 2; g++) {
		if (!groups[g])
			continue;
		for (int i = 0; groups[g][i * 2] && count < 8; i++) {
			char* button = groups[g][i * 2];
			char* hint = groups[g][i * 2 + 1];
			if (!hint)
				break;
			int w = GFX_getButtonWidth(hint, button);
			hints[count++] = (struct Hint){hint, button, w};
			total_w += SCALE1(BUTTON_MARGIN) + w;
		}
	}

	if (count == 0)
		return 0;
	total_w += SCALE1(BUTTON_MARGIN);

	// Full-width semi-transparent black bar
	int btn_sz = SCALE1(BUTTON_SIZE);
	int bar_h = btn_sz + SCALE1(BUTTON_MARGIN * 2);
	int oy = dst->h - bar_h;

	static SDL_Surface* button_bar = NULL;
	if (!button_bar || button_bar->w != dst->w || button_bar->h != bar_h) {
		if (button_bar)
			SDL_FreeSurface(button_bar);
		button_bar = SDL_CreateRGBSurface(SDL_SWSURFACE, dst->w, bar_h, 32,
										  0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
		if (!button_bar)
			return 0;
		SDL_FillRect(button_bar, NULL, SDL_MapRGBA(button_bar->format, 0, 0, 0, 178));
		SDL_SetSurfaceBlendMode(button_bar, SDL_BLENDMODE_BLEND);
	}
	SDL_BlitSurface(button_bar, NULL, dst, &(SDL_Rect){0, oy});

	// Render all buttons from the left
	int by = oy + (bar_h - btn_sz) / 2;
	int ox = SCALE1(PADDING) + SCALE1(BUTTON_MARGIN);
	for (int i = 0; i < count; i++) {
		GFX_blitButton(hints[i].hint, hints[i].button, dst, &(SDL_Rect){ox, by});
		ox += hints[i].ow + SCALE1(BUTTON_MARGIN);
	}

	return total_w;
}

void UI_showSplashScreen(SDL_Surface* screen, const char* title) {
	GFX_clear(screen);

	SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
	if (title_text) {
		SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){(screen->w - title_text->w) / 2, screen->h / 2 - title_text->h});
		SDL_FreeSurface(title_text);
	}

	SDL_Surface* loading = TTF_RenderUTF8_Blended(font.small, "Loading...", COLOR_GRAY);
	if (loading) {
		SDL_BlitSurface(loading, NULL, screen, &(SDL_Rect){(screen->w - loading->w) / 2, screen->h / 2 + SCALE1(4)});
		SDL_FreeSurface(loading);
	}

	GFX_flip(screen);
}

void UI_renderLoadingOverlay(SDL_Surface* dst, const char* title, const char* subtitle) {
	// Full-screen semi-transparent overlay (cached, same pattern as button hint bar)
	static SDL_Surface* overlay = NULL;
	if (!overlay || overlay->w != dst->w || overlay->h != dst->h) {
		if (overlay)
			SDL_FreeSurface(overlay);
		overlay = SDL_CreateRGBSurface(SDL_SWSURFACE, dst->w, dst->h, 32,
									   0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
		if (!overlay)
			return;
		SDL_FillRect(overlay, NULL, SDL_MapRGBA(overlay->format, 0, 0, 0, 178));
		SDL_SetSurfaceBlendMode(overlay, SDL_BLENDMODE_BLEND);
	}
	SDL_BlitSurface(overlay, NULL, dst, &(SDL_Rect){0, 0});

	// Title: large font, white, centered
	int title_h = TTF_FontHeight(font.large);
	int total_h = title_h;
	if (subtitle)
		total_h += SCALE1(4) + TTF_FontHeight(font.small);
	int y = (dst->h - total_h) / 2;

	SDL_Rect title_rect = {0, y, dst->w, title_h};
	GFX_blitMessage(font.large, (char*)title, dst, &title_rect);

	// Subtitle: small font, gray, centered below title
	if (subtitle) {
		int sub_h = TTF_FontHeight(font.small);
		y += title_h + SCALE1(4);
		SDL_Rect sub_rect = {0, y, dst->w, sub_h};
		GFX_blitMessage(font.small, (char*)subtitle, dst, &sub_rect);
	}
}

int UI_renderMenuBar(SDL_Surface* screen, const char* title, int show_setting) {
	// Semi-transparent bar background (cached between calls)
	static SDL_Surface* menu_bar = NULL;
	int bar_h = SCALE1(BUTTON_SIZE) + SCALE1(BUTTON_MARGIN * 2);
	if (!menu_bar || menu_bar->w != screen->w || menu_bar->h != bar_h) {
		if (menu_bar)
			SDL_FreeSurface(menu_bar);
		menu_bar = SDL_CreateRGBSurface(SDL_SWSURFACE, screen->w, bar_h, 32,
										0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
		if (!menu_bar)
			return 0;
		SDL_FillRect(menu_bar, NULL, SDL_MapRGBA(menu_bar->format, 0, 0, 0, 178));
		SDL_SetSurfaceBlendMode(menu_bar, SDL_BLENDMODE_BLEND);
	}
	SDL_BlitSurface(menu_bar, NULL, screen, &(SDL_Rect){0, 0});

	// Hardware group (right side)
	int ow = GFX_blitHardwareGroup(screen, show_setting);

	// Title text (left side, no pill)
	if (title && title[0]) {
		int max_title_w = screen->w - ow - SCALE1(PADDING * 2);
		char truncated[256];
		GFX_truncateText(font.small, title, truncated, max_title_w, 0);

		SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, truncated, COLOR_GRAY);
		if (text) {
			int text_y = (bar_h - text->h) / 2;
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), text_y});
			SDL_FreeSurface(text);
		}
	}

	return ow;
}
