#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "api.h"
#include "config.h"
#include "ui_fonts.h"

// Get text color for list items based on selection state
SDL_Color Fonts_getListTextColor(bool selected) {
	return selected ? uintToColour(THEME_COLOR5_255) : uintToColour(THEME_COLOR4_255);
}

// Draw list item background pill (only draws if selected)
void Fonts_drawListItemBg(SDL_Surface* screen, SDL_Rect* rect, bool selected) {
	if (selected) {
		GFX_blitPillColor(ASSET_WHITE_PILL, screen, rect, THEME_COLOR1, RGB_WHITE);
	}
}

// Calculate pill width for list items
int Fonts_calcListPillWidth(TTF_Font* f, const char* text, char* truncated, int max_width, int prefix_width) {
	int available_width = max_width - prefix_width;
	int padding = SCALE1(BUTTON_PADDING * 2);

	// Check if text fits without truncation
	int raw_text_w, raw_text_h;
	TTF_SizeUTF8(f, text, &raw_text_w, &raw_text_h);

	if (raw_text_w + padding > available_width) {
		// Text needs truncation - extend pill to full width (no right padding gap)
		GFX_truncateText(f, text, truncated, available_width, padding);
		return max_width;
	}

	// Text fits - use actual text width with padding
	strncpy(truncated, text, 255);
	truncated[255] = '\0';
	return MIN(max_width, prefix_width + raw_text_w + padding);
}
