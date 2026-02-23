#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_components.h"
#include "ui_playlist.h"
#include "ui_icons.h"
#include "ui_list.h"

// Scroll text state for selected item in playlist lists
static ScrollTextState playlist_scroll = {0};

void render_playlist_list(SDL_Surface* screen, IndicatorType show_setting,
						  PlaylistInfo* playlists, int count,
						  int selected, int scroll) {
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;
	char truncated[256];

	UI_renderMenuBar(screen, "Playlists");

	// Empty state - no playlists
	if (count == 0) {
		UI_renderEmptyState(screen, "No playlists saved", "Press Y to create a playlist", "NEW");
		return;
	}

	ListLayout layout = UI_calcListLayout(screen);

	for (int i = 0; i < layout.items_per_page && (scroll + i) < count; i++) {
		int idx = scroll + i;
		bool is_selected = (idx == selected);
		int y = layout.list_y + i * layout.item_h;

		PlaylistInfo* pl = &playlists[idx];
		char display[256];
		snprintf(display, sizeof(display), "%s (%d)", pl->name, pl->track_count);

		ListItemPos pos = UI_renderListItemPill(screen, &layout, font.medium, display, truncated, y, is_selected, 0);
		int available_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2);
		UI_renderListItemText(screen, &playlist_scroll, display, font.medium,
							  pos.text_x, pos.text_y, available_width, is_selected);
	}

	UI_renderScrollIndicators(screen, scroll, layout.items_per_page, count);

	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "SELECT", NULL});
}

void render_playlist_detail(SDL_Surface* screen, IndicatorType show_setting,
							const char* playlist_name,
							PlaylistTrack* tracks, int count,
							int selected, int scroll) {
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;
	char truncated[256];

	char title[300];
	snprintf(title, sizeof(title), "Playlist %s", playlist_name);
	UI_renderMenuBar(screen, title);

	// Empty state
	if (count == 0) {
		UI_renderEmptyState(screen, "No tracks in playlist", "Add tracks from the music browser", NULL);
		return;
	}

	ListLayout layout = UI_calcListLayout(screen);

	// Icon size and spacing (same as browser)
	int icon_size = Icons_isLoaded() ? SCALE1(24) : 0;
	int icon_spacing = Icons_isLoaded() ? SCALE1(6) : 0;
	int icon_offset = icon_size + icon_spacing;

	for (int i = 0; i < layout.items_per_page && (scroll + i) < count; i++) {
		int idx = scroll + i;
		bool is_selected = (idx == selected);
		int y = layout.list_y + i * layout.item_h;

		char display[256];
		PlaylistTrack* track = &tracks[idx];
		snprintf(display, sizeof(display), "%s", track->name);

		ListItemPos pos = UI_renderListItemPill(screen, &layout, font.medium, display, truncated, y, is_selected, icon_offset);

		// Render icon
		if (Icons_isLoaded()) {
			SDL_Surface* icon = Icons_getForFormat(tracks[idx].format, is_selected);
			if (icon) {
				int icon_y = y + (layout.item_h - icon_size) / 2;
				SDL_Rect src_rect = {0, 0, icon->w, icon->h};
				SDL_Rect dst_rect = {pos.text_x, icon_y, icon_size, icon_size};
				SDL_BlitScaled(icon, &src_rect, screen, &dst_rect);
			}
		}

		int text_x = pos.text_x + icon_offset;
		int available_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2) - icon_offset;
		UI_renderListItemText(screen, &playlist_scroll, display, font.medium,
							  text_x, pos.text_y, available_width, is_selected);
	}

	UI_renderScrollIndicators(screen, scroll, layout.items_per_page, count);
}

bool playlist_list_needs_scroll_refresh(void) {
	return ScrollText_isScrolling(&playlist_scroll);
}

bool playlist_list_scroll_needs_render(void) {
	return ScrollText_needsRender(&playlist_scroll);
}

void playlist_list_animate_scroll(void) {
	ScrollText_animateOnly(&playlist_scroll);
}
