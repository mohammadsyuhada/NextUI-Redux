#include <stdio.h>
#include <string.h>
#include "defines.h"
#include "api.h"
#include "ui_components.h"
#include "ui_music.h"
#include "ui_icons.h"
#include "ui_utils.h"
#include "ui_list.h"
#include "ui_album_art.h"
#include "spectrum.h"
#include "lyrics.h"
#include "settings.h"

// Scroll text state for browser list (selected item)
static ScrollTextState browser_scroll = {0};

// Scroll text state for player title
static ScrollTextState player_title_scroll;

// Playtime GPU state
static int playtime_x = 0, playtime_y = 0, playtime_dur_x = 0;
static int last_rendered_position = -1;
static int last_rendered_duration = -1;
static bool playtime_position_set = false;

// Lyrics GPU state
static int lyrics_gpu_x = 0, lyrics_gpu_y = 0, lyrics_gpu_max_w = 0;
static bool lyrics_gpu_position_set = false;
static char last_lyric_line[256] = "";
static char last_next_lyric_line[256] = "";

// Render the file browser
void render_browser(SDL_Surface* screen, IndicatorType show_setting, BrowserContext* browser) {
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;
	char truncated[256];

	UI_renderMenuBar(screen, "Music Player");

	// Empty state at root: no playable music anywhere
	if (Browser_countAudioFiles(browser) == 0 && !Browser_hasParent(browser)) {
		if (!Browser_hasAudioRecursive(browser->current_path)) {
			UI_renderEmptyState(screen, "No music files found", "Add music to /Music on your SD card", NULL);
			return;
		}
	}

	// Use common list layout calculation
	ListLayout layout = UI_calcListLayout(screen);
	browser->items_per_page = layout.items_per_page;

	UI_adjustListScroll(browser->selected, &browser->scroll_offset, browser->items_per_page);

	// Calculate icon size and spacing (icons are 24x24)
	int icon_size = Icons_isLoaded() ? SCALE1(24) : 0;
	int icon_spacing = Icons_isLoaded() ? SCALE1(6) : 0;
	int icon_offset = icon_size + icon_spacing;

	for (int i = 0; i < browser->items_per_page && browser->scroll_offset + i < browser->entry_count; i++) {
		int idx = browser->scroll_offset + i;
		FileEntry* entry = &browser->entries[idx];
		bool selected = (idx == browser->selected);

		int y = layout.list_y + i * layout.item_h;

		// Get display name (without folder brackets or prefixes when icons are used)
		char display[256];
		if (Icons_isLoaded()) {
			// With icons, use clean names
			if (entry->is_dir || entry->is_play_all) {
				strncpy(display, entry->name, sizeof(display) - 1);
				display[sizeof(display) - 1] = '\0';
			} else {
				Browser_getDisplayName(entry->name, display, sizeof(display));
			}
		} else {
			// Without icons, use text indicators
			if (entry->is_dir) {
				snprintf(display, sizeof(display), "[%s]", entry->name);
			} else if (entry->is_play_all) {
				snprintf(display, sizeof(display), "> %s", entry->name);
			} else {
				Browser_getDisplayName(entry->name, display, sizeof(display));
			}
		}

		// Render pill background and get text position (with icon offset)
		ListItemPos pos = UI_renderListItemPill(screen, &layout, font.medium, display, truncated, y, selected, icon_offset);

		// Render icon if available
		if (Icons_isLoaded()) {
			SDL_Surface* icon = NULL;
			if (entry->is_dir) {
				icon = Icons_getFolder(selected);
			} else if (entry->is_play_all) {
				icon = Icons_getPlayAll(selected);
			} else {
				icon = Icons_getForFormat(entry->format, selected);
			}

			if (icon) {
				// Center icon vertically within the item
				int icon_y = y + (layout.item_h - icon_size) / 2;
				int icon_x = pos.text_x;

				// Scale and blit the icon
				SDL_Rect src_rect = {0, 0, icon->w, icon->h};
				SDL_Rect dst_rect = {icon_x, icon_y, icon_size, icon_size};
				SDL_BlitScaled(icon, &src_rect, screen, &dst_rect);
			}
		}

		// Adjust text position for icon
		int text_x = pos.text_x + icon_offset;
		int available_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2) - icon_offset;

		// Use common text rendering with scrolling for selected items
		UI_renderListItemText(screen, &browser_scroll, display, font.medium,
							  text_x, pos.text_y, available_width, selected);
	}

	UI_renderScrollIndicators(screen, browser->scroll_offset, browser->items_per_page, browser->entry_count);

	// Button hints
	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "SELECT", NULL});
}

// Render the now playing screen
void render_playing(SDL_Surface* screen, IndicatorType show_setting, BrowserContext* browser,
					bool shuffle_enabled, bool repeat_enabled,
					int playlist_track_num, int playlist_total) {
	GFX_clear(screen);

	// Render album art as triangular background (if available)
	SDL_Surface* album_art = Player_getAlbumArt();
	if (album_art && album_art->w > 0 && album_art->h > 0) {
		render_album_art_background(screen, album_art);
	}

	int hw = screen->w;
	int hh = screen->h;

	const TrackInfo* info = Player_getTrackInfo();
	PlayerState state = Player_getState();
	AudioFormat format = Player_detectFormat(Player_getCurrentFile());
	int duration = Player_getDuration();
	int position = Player_getPosition();
	float progress = (duration > 0) ? (float)position / duration : 0.0f;

	// === TOP BAR ===
	int top_y = SCALE1(PADDING);

	// Format badge "FLAC" with border (smaller, gray) - render first on the left
	const char* fmt_name = get_format_name(format);
	SDL_Surface* fmt_surf = TTF_RenderUTF8_Blended(font.tiny, fmt_name, COLOR_GRAY);
	int badge_h = fmt_surf ? fmt_surf->h + SCALE1(4) : SCALE1(16);
	int badge_x = SCALE1(PADDING);
	int badge_w = 0;

	// Draw format badge on the left
	if (fmt_surf) {
		badge_w = fmt_surf->w + SCALE1(10);
		// Draw border (gray)
		SDL_Rect border = {badge_x, top_y, badge_w, badge_h};
		SDL_FillRect(screen, &border, RGB_GRAY);
		SDL_Rect inner = {badge_x + 1, top_y + 1, badge_w - 2, badge_h - 2};
		SDL_FillRect(screen, &inner, RGB_BLACK);
		SDL_BlitSurface(fmt_surf, NULL, screen, &(SDL_Rect){badge_x + SCALE1(5), top_y + SCALE1(2)});
		SDL_FreeSurface(fmt_surf);
	}

	// Track counter "01 - 03" (smaller, gray) - after the format badge
	// Use playlist counts if available (playlist_total > 0), otherwise use browser counts
	int track_num = (playlist_total > 0) ? playlist_track_num : Browser_getCurrentTrackNumber(browser);
	int total_tracks = (playlist_total > 0) ? playlist_total : Browser_countAudioFiles(browser);
	char track_str[32];
	snprintf(track_str, sizeof(track_str), "%02d - %02d", track_num, total_tracks);
	SDL_Surface* track_surf = TTF_RenderUTF8_Blended(font.tiny, track_str, COLOR_GRAY);
	if (track_surf) {
		int track_x = badge_x + badge_w + SCALE1(8);
		int track_y = top_y + (badge_h - track_surf->h) / 2;
		SDL_BlitSurface(track_surf, NULL, screen, &(SDL_Rect){track_x, track_y});
		SDL_FreeSurface(track_surf);
	}

	// Hardware status (clock, battery) on right
	GFX_blitHardwareGroup(screen, show_setting);

	// === TRACK INFO SECTION ===
	int info_y = SCALE1(PADDING + 45);
	char truncated[256];

	// Max width for text (album art is now only shown as background)
	int max_w_text = hw - SCALE1(PADDING * 2);

	// Artist name (Medium font, gray)
	const char* artist = info->artist[0] ? info->artist : "Unknown Artist";
	GFX_truncateText(font.medium, artist, truncated, max_w_text, 0);
	SDL_Surface* artist_surf = TTF_RenderUTF8_Blended(font.medium, truncated, COLOR_GRAY);
	if (artist_surf) {
		SDL_BlitSurface(artist_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
		info_y += artist_surf->h + SCALE1(2);
		SDL_FreeSurface(artist_surf);
	} else {
		info_y += SCALE1(18);
	}

	// Song title (Regular font extra large, white) - with GPU scrolling animation (no background)
	const char* title = info->title[0] ? info->title : "Unknown Title";
	int title_y = info_y; // Save for GPU scroll

	// Check if text changed and reset scroll state
	if (strcmp(player_title_scroll.text, title) != 0) {
		ScrollText_reset(&player_title_scroll, title, font.title, max_w_text, true); // true = GPU mode
	}

	// Activate scroll after delay (this render path bypasses ScrollText_render)
	ScrollText_activateAfterDelay(&player_title_scroll);

	// If text needs scrolling, use GPU layer (no background)
	if (player_title_scroll.needs_scroll) {
		ScrollText_renderGPU_NoBg(&player_title_scroll, font.title, COLOR_WHITE, SCALE1(PADDING), title_y);
	} else {
		// Static text - render to screen surface
		PLAT_clearLayers(LAYER_SCROLLTEXT);
		SDL_Surface* title_surf = TTF_RenderUTF8_Blended(font.title, title, COLOR_WHITE);
		if (title_surf) {
			SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), title_y, 0, 0});
			SDL_FreeSurface(title_surf);
		}
	}
	info_y += TTF_FontHeight(font.title) + SCALE1(2);

	// Lyric lines (GPU rendered) or album name (screen rendered)
	if (Settings_getLyricsEnabled()) {
		Lyrics_setGPUPosition(SCALE1(PADDING), info_y, max_w_text);
	} else {
		Lyrics_clearGPU();
		// Show album name when lyrics are off
		const char* album = info->album[0] ? info->album : "";
		if (album[0]) {
			GFX_truncateText(font.small, album, truncated, max_w_text, 0);
			SDL_Surface* album_surf = TTF_RenderUTF8_Blended(font.small, truncated, COLOR_GRAY);
			if (album_surf) {
				SDL_BlitSurface(album_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
				SDL_FreeSurface(album_surf);
			}
		}
	}

	// === SPECTRUM SECTION (GPU rendered) ===
	int spec_y = hh - SCALE1(90);
	int spec_h = SCALE1(50);
	int spec_x = SCALE1(PADDING);
	int spec_w = hw - SCALE1(PADDING * 2);

	// Set position for GPU rendering (actual rendering happens in main loop)
	Spectrum_setPosition(spec_x, spec_y, spec_w, spec_h);

	// === BOTTOM BAR ===
	int bottom_y = hh - SCALE1(35);

	// Time display is rendered via GPU layer - just set position here
	// Calculate position based on font metrics
	int time_x = SCALE1(PADDING);
	// Duration x position will be calculated in GPU render based on actual position text width
	PlayTime_setPosition(time_x, bottom_y, 0);

	// Shuffle and Repeat labels on right side
	int label_x = hw - SCALE1(PADDING);

	// Repeat label
	const char* repeat_text = "REPEAT";
	SDL_Color repeat_color = repeat_enabled ? COLOR_WHITE : COLOR_GRAY;
	SDL_Surface* repeat_surf = TTF_RenderUTF8_Blended(font.tiny, repeat_text, repeat_color);
	if (repeat_surf) {
		label_x -= repeat_surf->w;
		SDL_BlitSurface(repeat_surf, NULL, screen, &(SDL_Rect){label_x, bottom_y});
		// Draw underline if enabled
		if (repeat_enabled) {
			SDL_Rect underline = {label_x, bottom_y + repeat_surf->h, repeat_surf->w, SCALE1(1)};
			SDL_FillRect(screen, &underline, RGB_WHITE);
		}
		SDL_FreeSurface(repeat_surf);
	}

	// Shuffle label (with gap before repeat)
	label_x -= SCALE1(12);
	const char* shuffle_text = "SHUFFLE";
	SDL_Color shuffle_color = shuffle_enabled ? COLOR_WHITE : COLOR_GRAY;
	SDL_Surface* shuffle_surf = TTF_RenderUTF8_Blended(font.tiny, shuffle_text, shuffle_color);
	if (shuffle_surf) {
		label_x -= shuffle_surf->w;
		SDL_BlitSurface(shuffle_surf, NULL, screen, &(SDL_Rect){label_x, bottom_y});
		// Draw underline if enabled
		if (shuffle_enabled) {
			SDL_Rect underline = {label_x, bottom_y + shuffle_surf->h, shuffle_surf->w, SCALE1(1)};
			SDL_FillRect(screen, &underline, RGB_WHITE);
		}
		SDL_FreeSurface(shuffle_surf);
	}

	// Lyric Off label (only shown when lyrics are disabled)
	if (!Settings_getLyricsEnabled()) {
		label_x -= SCALE1(12);
		const char* lyric_text = "LYRIC OFF";
		SDL_Surface* lyric_surf = TTF_RenderUTF8_Blended(font.tiny, lyric_text, COLOR_GRAY);
		if (lyric_surf) {
			label_x -= lyric_surf->w;
			SDL_BlitSurface(lyric_surf, NULL, screen, &(SDL_Rect){label_x, bottom_y});
			SDL_FreeSurface(lyric_surf);
		}
	}
}

// Check if browser list has active scrolling (for refresh optimization)
bool browser_needs_scroll_refresh(void) {
	return ScrollText_isScrolling(&browser_scroll);
}

// Check if browser scroll needs a render to transition (delay phase)
bool browser_scroll_needs_render(void) {
	return ScrollText_needsRender(&browser_scroll);
}

// Animate browser scroll only (GPU mode, no screen redraw needed)
void browser_animate_scroll(void) {
	ScrollText_animateOnly(&browser_scroll);
}

// Check if player title has active scrolling (for refresh optimization)
bool player_needs_scroll_refresh(void) {
	// Only scroll when playing, not when paused
	if (Player_getState() != PLAYER_STATE_PLAYING)
		return false;
	return ScrollText_isScrolling(&player_title_scroll);
}

// Check if player title scroll needs a render to transition (delay phase)
bool player_title_scroll_needs_render(void) {
	return ScrollText_needsRender(&player_title_scroll);
}

// Animate player title scroll (GPU mode, no screen redraw needed)
void player_animate_scroll(void) {
	if (!player_title_scroll.text[0] || !player_title_scroll.needs_scroll)
		return;
	ScrollText_renderGPU_NoBg(&player_title_scroll, player_title_scroll.last_font,
							  player_title_scroll.last_color,
							  player_title_scroll.last_x, player_title_scroll.last_y);
}

// === PLAYTIME GPU FUNCTIONS ===

void PlayTime_setPosition(int x, int y, int duration_x) {
	playtime_x = x;
	playtime_y = y;
	playtime_dur_x = duration_x;
	playtime_position_set = true;
}

void PlayTime_clear(void) {
	playtime_position_set = false;
	last_rendered_position = -1;
	last_rendered_duration = -1;
	// Note: Caller should clear LAYER_PLAYTIME and call PLAT_GPU_Flip() if needed
}

bool PlayTime_needsRefresh(void) {
	if (!playtime_position_set)
		return false;
	// Only update when playing, not when paused
	if (Player_getState() != PLAYER_STATE_PLAYING)
		return false;

	int position = Player_getPosition();
	int duration = Player_getDuration();

	// Only refresh if position changed (updates once per second)
	return (position != last_rendered_position || duration != last_rendered_duration);
}

void PlayTime_renderGPU(void) {
	if (!playtime_position_set)
		return;

	int position = Player_getPosition();
	int duration = Player_getDuration();

	// Skip if nothing changed
	if (position == last_rendered_position && duration == last_rendered_duration)
		return;

	last_rendered_position = position;
	last_rendered_duration = duration;

	// Render position text
	char pos_str[16];
	format_time(pos_str, position);
	SDL_Surface* pos_surf = TTF_RenderUTF8_Blended(font.small, pos_str, COLOR_WHITE);
	if (!pos_surf)
		return;

	// Render duration text
	char dur_str[16];
	format_time(dur_str, duration);
	SDL_Surface* dur_surf = TTF_RenderUTF8_Blended(font.tiny, dur_str, COLOR_GRAY);

	// Calculate total width needed
	int total_w = pos_surf->w + SCALE1(6) + (dur_surf ? dur_surf->w : 0);
	int total_h = pos_surf->h;

	// Create combined surface
	SDL_Surface* combined = SDL_CreateRGBSurfaceWithFormat(0, total_w, total_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (combined) {
		SDL_FillRect(combined, NULL, 0); // Transparent background

		// Blit position
		SDL_BlitSurface(pos_surf, NULL, combined, &(SDL_Rect){0, 0, 0, 0});

		// Blit duration (aligned to bottom of position text)
		if (dur_surf) {
			int dur_y = pos_surf->h - dur_surf->h;
			SDL_BlitSurface(dur_surf, NULL, combined, &(SDL_Rect){pos_surf->w + SCALE1(6), dur_y, 0, 0});
		}

		// Clear previous and draw new
		PLAT_clearLayers(LAYER_PLAYTIME);
		PLAT_drawOnLayer(combined, playtime_x, playtime_y, total_w, total_h, 1.0f, false, LAYER_PLAYTIME);
		SDL_FreeSurface(combined);

		PLAT_GPU_Flip();
	}

	SDL_FreeSurface(pos_surf);
	if (dur_surf)
		SDL_FreeSurface(dur_surf);
}

// === LYRICS GPU FUNCTIONS ===

void Lyrics_setGPUPosition(int x, int y, int max_w) {
	lyrics_gpu_x = x;
	lyrics_gpu_y = y;
	lyrics_gpu_max_w = max_w;
	lyrics_gpu_position_set = true;
}

void Lyrics_clearGPU(void) {
	lyrics_gpu_position_set = false;
	last_lyric_line[0] = '\0';
	last_next_lyric_line[0] = '\0';
	PLAT_clearLayers(LAYER_LYRICS);
}

bool Lyrics_GPUneedsRefresh(void) {
	if (!lyrics_gpu_position_set || !Settings_getLyricsEnabled())
		return false;
	if (Player_getState() != PLAYER_STATE_PLAYING)
		return false;

	const char* current = Lyrics_getCurrentLine(Player_getPosition());
	const char* next = Lyrics_getNextLine();
	const char* cur_str = current ? current : "";
	const char* next_str = next ? next : "";

	return (strcmp(cur_str, last_lyric_line) != 0 || strcmp(next_str, last_next_lyric_line) != 0);
}

void Lyrics_renderGPU(void) {
	if (!lyrics_gpu_position_set || !Settings_getLyricsEnabled())
		return;

	const char* current = Lyrics_getCurrentLine(Player_getPosition());
	const char* next = Lyrics_getNextLine();
	const char* cur_str = current ? current : "";
	const char* next_str = next ? next : "";

	// Skip if nothing changed
	if (strcmp(cur_str, last_lyric_line) == 0 && strcmp(next_str, last_next_lyric_line) == 0)
		return;

	strncpy(last_lyric_line, cur_str, sizeof(last_lyric_line) - 1);
	last_lyric_line[sizeof(last_lyric_line) - 1] = '\0';
	strncpy(last_next_lyric_line, next_str, sizeof(last_next_lyric_line) - 1);
	last_next_lyric_line[sizeof(last_next_lyric_line) - 1] = '\0';

	char truncated[256];
	int line_h = TTF_FontHeight(font.small);
	bool has_cur = (cur_str[0] != '\0');
	bool has_next = (next_str[0] != '\0');

	// Nothing to show — clear layer
	if (!has_cur && !has_next) {
		PLAT_clearLayers(LAYER_LYRICS);
		PLAT_GPU_Flip();
		return;
	}

	// Calculate total height based on what's actually shown
	int total_h = 0;
	if (has_cur)
		total_h += line_h;
	if (has_cur && has_next)
		total_h += SCALE1(2);
	if (has_next)
		total_h += line_h;

	// Render current line
	SDL_Surface* cur_surf = NULL;
	if (has_cur) {
		GFX_truncateText(font.small, cur_str, truncated, lyrics_gpu_max_w, 0);
		cur_surf = TTF_RenderUTF8_Blended(font.small, truncated, COLOR_LIGHT_TEXT);
	}

	// Render next line
	SDL_Surface* next_surf = NULL;
	if (has_next) {
		GFX_truncateText(font.small, next_str, truncated, lyrics_gpu_max_w, 0);
		next_surf = TTF_RenderUTF8_Blended(font.small, truncated, COLOR_DARK_TEXT);
	}

	// Create combined surface
	SDL_Surface* combined = SDL_CreateRGBSurfaceWithFormat(0, lyrics_gpu_max_w, total_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (combined) {
		SDL_FillRect(combined, NULL, 0); // Transparent background

		int y_offset = 0;
		if (cur_surf) {
			SDL_BlitSurface(cur_surf, NULL, combined, &(SDL_Rect){0, y_offset, 0, 0});
			y_offset += line_h + SCALE1(2);
		}
		if (next_surf) {
			SDL_BlitSurface(next_surf, NULL, combined, &(SDL_Rect){0, y_offset, 0, 0});
		}

		PLAT_clearLayers(LAYER_LYRICS);
		PLAT_drawOnLayer(combined, lyrics_gpu_x, lyrics_gpu_y, lyrics_gpu_max_w, total_h, 1.0f, false, LAYER_LYRICS);
		SDL_FreeSurface(combined);

		PLAT_GPU_Flip();
	}

	if (cur_surf)
		SDL_FreeSurface(cur_surf);
	if (next_surf)
		SDL_FreeSurface(next_surf);
}
