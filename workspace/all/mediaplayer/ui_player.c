#include <stdio.h>
#include <string.h>

#include "vp_defines.h"
#include "api.h"
#include "ui_components.h"
#include "ui_player.h"
#include "ui_icons.h"
#include "video_browser.h"

// Render the video file browser
void render_video_browser(SDL_Surface* screen, IndicatorType show_setting,
						  VideoBrowserContext* ctx, ScrollTextState* scroll) {
	GFX_clear(screen);

	char truncated[256];

	// Determine header title: "Videos" at root, or folder name in subdirectories
	const char* header_title = "Videos";
	if (strcmp(ctx->current_path, VIDEO_ROOT) != 0) {
		const char* slash = strrchr(ctx->current_path, '/');
		if (slash && slash[1] != '\0') {
			header_title = slash + 1;
		}
	}

	UI_renderMenuBar(screen, header_title);

	// Empty state: no videos at root
	if (ctx->entry_count == 0) {
		UI_renderEmptyState(screen, "No videos found",
							"Add videos to /Videos on your SD card", NULL);
		return;
	}

	// Calculate list layout
	ListLayout layout = UI_calcListLayout(screen);
	ctx->items_per_page = layout.items_per_page;

	// Adjust scroll to keep selected item visible
	UI_adjustListScroll(ctx->selected, &ctx->scroll_offset, ctx->items_per_page);

	// Icon dimensions
	int icon_size = Icons_isLoaded() ? SCALE1(24) : 0;
	int icon_spacing = Icons_isLoaded() ? SCALE1(6) : 0;

	// Render visible entries
	for (int i = 0; i < ctx->items_per_page && (ctx->scroll_offset + i) < ctx->entry_count; i++) {
		int idx = ctx->scroll_offset + i;
		VideoFileEntry* entry = &ctx->entries[idx];
		bool selected = (idx == ctx->selected);

		int y = layout.list_y + i * layout.item_h;

		// Prepare display name
		char display[256];
		if (entry->is_dir) {
			// Directory: show raw name (icons differentiate, or brackets if no icons)
			if (Icons_isLoaded()) {
				strncpy(display, entry->name, sizeof(display) - 1);
				display[sizeof(display) - 1] = '\0';
			} else {
				snprintf(display, sizeof(display), "[%s]", entry->name);
			}
		} else {
			// Video file: strip extension for display
			VideoBrowser_getDisplayName(entry->name, display, sizeof(display));
		}

		// Calculate icon offset for pill width
		int icon_offset = Icons_isLoaded() ? (icon_size + icon_spacing) : 0;

		// Render pill background and get text position
		ListItemPos pos = UI_renderListItemPill(screen, &layout, font.medium, display, truncated,
												y, selected, icon_offset);

		// Render icon
		if (Icons_isLoaded()) {
			SDL_Surface* icon = NULL;
			if (entry->is_dir) {
				icon = Icons_getFolder(selected);
			} else {
				icon = Icons_getForFormat(entry->format, selected);
			}
			if (icon) {
				int icon_y = y + (layout.item_h - icon_size) / 2;
				SDL_Rect src_rect = {0, 0, icon->w, icon->h};
				SDL_Rect dst_rect = {pos.text_x, icon_y, icon_size, icon_size};
				SDL_BlitScaled(icon, &src_rect, screen, &dst_rect);
			}
		}

		// Calculate text position (after icon if present)
		int text_x = pos.text_x + icon_offset;
		int available_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2) - icon_offset;

		// Render text with scrolling for selected item
		UI_renderListItemText(screen, scroll, display, font.medium,
							  text_x, pos.text_y, available_width, selected);
	}

	// Scroll indicators (up/down arrows)
	UI_renderScrollIndicators(screen, ctx->scroll_offset, ctx->items_per_page, ctx->entry_count);

	// Button hints
	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "OPEN", NULL});
}

// Render loading/buffering screen
void render_loading_screen(SDL_Surface* screen, const char* message) {
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;

	const char* text = message ? message : "Loading...";

	SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font.medium, text, COLOR_WHITE);
	if (text_surf) {
		int x = (hw - text_surf->w) / 2;
		int y = (hh - text_surf->h) / 2;
		SDL_BlitSurface(text_surf, NULL, screen, &(SDL_Rect){x, y, 0, 0});
		SDL_FreeSurface(text_surf);
	}

	GFX_flip(screen);
}
