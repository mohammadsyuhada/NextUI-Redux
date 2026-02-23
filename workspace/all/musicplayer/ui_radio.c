#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_components.h"
#include "ui_radio.h"
#include "ui_fonts.h"
#include "ui_utils.h"
#include "ui_toast.h"
#include "ui_album_art.h"
#include "album_art.h"
#include "radio_curated.h"

// Render the radio station list
void render_radio_list(SDL_Surface* screen, IndicatorType show_setting,
					   int radio_selected, int* radio_scroll,
					   const char* toast_message, uint32_t toast_time) {
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;
	char truncated[256];

	UI_renderMenuBar(screen, "Online Radio");

	// Station list
	RadioStation* stations;
	int station_count = Radio_getStations(&stations);

	// Empty state - no stations saved
	if (station_count == 0) {
		UI_renderEmptyState(screen, "No stations saved", "Press Y to manage stations", "MANAGE");
		return;
	}

	// Use common list layout calculation
	ListLayout layout = UI_calcListLayout(screen);
	adjust_list_scroll(radio_selected, radio_scroll, layout.items_per_page);

	for (int i = 0; i < layout.items_per_page && *radio_scroll + i < station_count; i++) {
		int idx = *radio_scroll + i;
		RadioStation* station = &stations[idx];
		bool selected = (idx == radio_selected);

		int y = layout.list_y + i * layout.item_h;

		// Render pill background and get text position
		ListItemPos pos = UI_renderListItemPill(screen, &layout, font.medium, station->name, truncated, y, selected, 0);

		// Station name (no scrolling for radio list)
		UI_renderListItemText(screen, NULL, station->name, font.medium,
							  pos.text_x, pos.text_y, layout.max_width, selected);

		// Genre (if available)
		if (station->genre[0]) {
			SDL_Color genre_color = selected ? COLOR_GRAY : COLOR_DARK_TEXT;
			SDL_Surface* genre_text = TTF_RenderUTF8_Blended(font.tiny, station->genre, genre_color);
			if (genre_text) {
				SDL_BlitSurface(genre_text, NULL, screen, &(SDL_Rect){hw - genre_text->w - SCALE1(PADDING * 2), y + (layout.item_h - genre_text->h) / 2});
				SDL_FreeSurface(genre_text);
			}
		}
	}

	render_scroll_indicators(screen, *radio_scroll, layout.items_per_page, station_count);

	// Show note for users using default stations (no custom stations yet)
	if (!Radio_hasUserStations()) {
		int note_y = hh - SCALE1(BUTTON_SIZE + BUTTON_MARGIN + PADDING + 55);

		const char* note1 = "These are default stations";
		SDL_Surface* note1_surf = TTF_RenderUTF8_Blended(font.tiny, note1, COLOR_GRAY);
		if (note1_surf) {
			SDL_BlitSurface(note1_surf, NULL, screen, &(SDL_Rect){(hw - note1_surf->w) / 2, note_y});
			SDL_FreeSurface(note1_surf);
		}

		const char* note2 = "Press Y to manage stations";
		SDL_Surface* note2_surf = TTF_RenderUTF8_Blended(font.tiny, note2, COLOR_GRAY);
		if (note2_surf) {
			SDL_BlitSurface(note2_surf, NULL, screen, &(SDL_Rect){(hw - note2_surf->w) / 2, note_y + SCALE1(14)});
			SDL_FreeSurface(note2_surf);
		}
	}

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);

	// Button hints
	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "PLAY", NULL});
}

// Helper to get current station by index
static RadioStation* get_station_by_index(int index) {
	RadioStation* stations;
	int count = Radio_getStations(&stations);
	if (count > 0 && index < count) {
		return &stations[index];
	}
	return NULL;
}

// Render the radio playing screen
void render_radio_playing(SDL_Surface* screen, IndicatorType show_setting, int radio_selected) {
	GFX_clear(screen);

	// Render album art as triangular background (if available and not being fetched)
	// Skip during fetch to avoid accessing potentially invalid surface
	if (!album_art_is_fetching()) {
		SDL_Surface* album_art = Radio_getAlbumArt();
		if (album_art && album_art->w > 0 && album_art->h > 0) {
			render_album_art_background(screen, album_art);
		}
	}

	int hw = screen->w;
	int hh = screen->h;
	char truncated[256];

	RadioState state = Radio_getState();
	const RadioMetadata* meta = Radio_getMetadata();
	RadioStation* current_station = get_station_by_index(radio_selected);
	RadioStation* stations;
	int station_count = Radio_getStations(&stations);

	// === TOP BAR ===
	int top_y = SCALE1(PADDING);

	// "RADIO" badge with border (like format badge in local player)
	const char* badge_text = "RADIO";
	SDL_Surface* badge_surf = TTF_RenderUTF8_Blended(font.tiny, badge_text, COLOR_GRAY);
	int badge_h = badge_surf ? badge_surf->h + SCALE1(4) : SCALE1(16);
	int badge_x = SCALE1(PADDING);
	int badge_w = 0;

	if (badge_surf) {
		badge_w = badge_surf->w + SCALE1(10);
		// Draw border (gray)
		SDL_Rect border = {badge_x, top_y, badge_w, badge_h};
		SDL_FillRect(screen, &border, RGB_GRAY);
		SDL_Rect inner = {badge_x + 1, top_y + 1, badge_w - 2, badge_h - 2};
		SDL_FillRect(screen, &inner, RGB_BLACK);
		SDL_BlitSurface(badge_surf, NULL, screen, &(SDL_Rect){badge_x + SCALE1(5), top_y + SCALE1(2)});
		SDL_FreeSurface(badge_surf);
	}

	// Station counter "01 - 12" (like track counter in local player)
	char station_str[32];
	snprintf(station_str, sizeof(station_str), "%02d - %02d", radio_selected + 1, station_count);
	SDL_Surface* station_surf = TTF_RenderUTF8_Blended(font.tiny, station_str, COLOR_GRAY);
	if (station_surf) {
		int station_x = badge_x + badge_w + SCALE1(8);
		int station_y = top_y + (badge_h - station_surf->h) / 2;
		SDL_BlitSurface(station_surf, NULL, screen, &(SDL_Rect){station_x, station_y});
		SDL_FreeSurface(station_surf);
	}

	// Hardware status (clock, battery) on right
	GFX_blitHardwareGroup(screen, show_setting);

	// === STATION INFO SECTION ===
	int info_y = SCALE1(PADDING + 45);

	// Max widths for text (album art is now only shown as background)
	int max_w_half = (hw - SCALE1(PADDING * 2)) / 2;
	int max_w_full = hw - SCALE1(PADDING * 2);

	// Genre (like Artist in local player) - gray, medium font
	const char* genre = (current_station && current_station->genre[0]) ? current_station->genre : "Radio";
	GFX_truncateText(font.medium, genre, truncated, max_w_half, 0);
	SDL_Surface* genre_surf = TTF_RenderUTF8_Blended(font.medium, truncated, COLOR_GRAY);
	if (genre_surf) {
		SDL_BlitSurface(genre_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
		info_y += genre_surf->h + SCALE1(2);
		SDL_FreeSurface(genre_surf);
	} else {
		info_y += SCALE1(18);
	}

	// Station name (extra large font) - white
	const char* station_name = meta->station_name[0] ? meta->station_name : (current_station ? current_station->name : "Unknown Station");
	GFX_truncateText(font.xlarge, station_name, truncated, max_w_full, 0);
	SDL_Surface* name_surf = TTF_RenderUTF8_Blended(font.xlarge, truncated, COLOR_WHITE);
	if (name_surf) {
		SDL_BlitSurface(name_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
		info_y += name_surf->h + SCALE1(2);
		SDL_FreeSurface(name_surf);
	} else {
		info_y += SCALE1(40);
	}

	// Now Playing - Title on top (white, large), Artist below (gray, small)
	if (meta->title[0]) {
		// Title with text wrapping (max 3 lines)
		TTF_Font* title_font = font.medium;
		const char* src = meta->title;
		int max_lines = 3;
		int lines_rendered = 0;

		while (*src && lines_rendered < max_lines) {
			// Find how many characters fit on this line
			int text_len = strlen(src);
			int char_count = text_len;

			// Binary search for characters that fit
			while (char_count > 0) {
				char line_buf[256];
				int copy_len = (char_count < 255) ? char_count : 255;
				strncpy(line_buf, src, copy_len);
				line_buf[copy_len] = '\0';

				int w, h;
				TTF_SizeUTF8(title_font, line_buf, &w, &h);
				if (w <= max_w_full)
					break;
				char_count--;
			}

			if (char_count == 0)
				char_count = 1; // At least one character

			// Try to break at a space if not last line and not at end
			if (lines_rendered < max_lines - 1 && char_count < text_len) {
				int last_space = -1;
				for (int i = char_count - 1; i > 0; i--) {
					if (src[i] == ' ') {
						last_space = i;
						break;
					}
				}
				if (last_space > 0)
					char_count = last_space + 1;
			}

			// Render this line
			char line_buf[256];
			size_t copy_len = (char_count > 0 && char_count < 255) ? (size_t)char_count : 255;
			memcpy(line_buf, src, copy_len);
			line_buf[copy_len] = '\0';

			// Trim trailing space
			while (copy_len > 0 && line_buf[copy_len - 1] == ' ') {
				line_buf[--copy_len] = '\0';
			}

			if (strlen(line_buf) > 0) {
				SDL_Surface* title_surf = TTF_RenderUTF8_Blended(title_font, line_buf, COLOR_WHITE);
				if (title_surf) {
					SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
					info_y += title_surf->h + SCALE1(2);
					SDL_FreeSurface(title_surf);
				}
			}

			src += char_count;
			// Skip leading spaces on next line
			while (*src == ' ')
				src++;
			lines_rendered++;
		}
	}
	if (meta->artist[0]) {
		// Artist line (smaller font)
		GFX_truncateText(font.small, meta->artist, truncated, max_w_full, 0);
		SDL_Surface* artist_surf = TTF_RenderUTF8_Blended(font.small, truncated, COLOR_GRAY);
		if (artist_surf) {
			SDL_BlitSurface(artist_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
			info_y += artist_surf->h + SCALE1(2);
			SDL_FreeSurface(artist_surf);
		}
	}

	// Show slogan if no title/artist available
	if (!meta->title[0] && !meta->artist[0] && current_station && current_station->slogan[0]) {
		GFX_truncateText(font.small, current_station->slogan, truncated, max_w_full, 0);
		SDL_Surface* slogan_surf = TTF_RenderUTF8_Blended(font.small, truncated, COLOR_GRAY);
		if (slogan_surf) {
			SDL_BlitSurface(slogan_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
			info_y += slogan_surf->h + SCALE1(2);
			SDL_FreeSurface(slogan_surf);
		}
	}

	// Position for error message
	int vis_y = hh - SCALE1(90);

	// === BOTTOM BAR (GPU layer - position set here, rendering done independently) ===
	int bottom_y = hh - SCALE1(35);
	int bar_w = SCALE1(60);
	int bar_h = SCALE1(8);
	int bar_x = hw - SCALE1(PADDING) - bar_w;
	int bar_y = bottom_y + SCALE1(4);

	// Set position for GPU rendering (actual rendering happens in main loop)
	RadioStatus_setPosition(bar_x, bar_y, bar_w, bar_h, SCALE1(PADDING), bottom_y);

	// Error message (displayed prominently if in error state)
	if (state == RADIO_STATE_ERROR) {
		SDL_Surface* err_text = TTF_RenderUTF8_Blended(font.small, Radio_getError(), (SDL_Color){255, 100, 100, 255});
		if (err_text) {
			SDL_BlitSurface(err_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING), vis_y - SCALE1(20)});
			SDL_FreeSurface(err_text);
		}
	}
}

// Render add stations - country selection screen
void render_radio_add(SDL_Surface* screen, IndicatorType show_setting,
					  int add_country_selected, int* add_country_scroll) {
	GFX_clear(screen);

	int hw = screen->w;
	char truncated[256];

	UI_renderMenuBar(screen, "Manage Stations");

	// Country list
	int country_count = Radio_getCuratedCountryCount();
	const CuratedCountry* countries = Radio_getCuratedCountries();

	// Use common list layout calculation
	ListLayout layout = UI_calcListLayout(screen);
	adjust_list_scroll(add_country_selected, add_country_scroll, layout.items_per_page);

	for (int i = 0; i < layout.items_per_page && *add_country_scroll + i < country_count; i++) {
		int idx = *add_country_scroll + i;
		const CuratedCountry* country = &countries[idx];
		bool selected = (idx == add_country_selected);

		int y = layout.list_y + i * layout.item_h;

		// Render pill background and get text position
		ListItemPos pos = UI_renderListItemPill(screen, &layout, font.medium, country->name, truncated, y, selected, 0);

		// Country name
		UI_renderListItemText(screen, NULL, country->name, font.medium,
							  pos.text_x, pos.text_y, layout.max_width, selected);

		// Station count on right
		int curated_station_count = Radio_getCuratedStationCount(country->code);
		char count_str[32];
		snprintf(count_str, sizeof(count_str), "%d stations", curated_station_count);
		SDL_Color count_color = selected ? COLOR_GRAY : COLOR_DARK_TEXT;
		SDL_Surface* count_text = TTF_RenderUTF8_Blended(font.tiny, count_str, count_color);
		if (count_text) {
			SDL_BlitSurface(count_text, NULL, screen, &(SDL_Rect){hw - count_text->w - SCALE1(PADDING * 2), y + (layout.item_h - count_text->h) / 2});
			SDL_FreeSurface(count_text);
		}
	}

	render_scroll_indicators(screen, *add_country_scroll, layout.items_per_page, country_count);

	// Button hints
	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", "SELECT", NULL});
}

// Render add stations - station selection screen
void render_radio_add_stations(SDL_Surface* screen, IndicatorType show_setting,
							   const char* country_code,
							   int add_station_selected, int* add_station_scroll,
							   const int* sorted_indices, int sorted_count,
							   const char* toast_message, uint32_t toast_time) {
	GFX_clear(screen);

	int hw = screen->w;
	char truncated[256];

	// Get country name for title
	const char* country_name = "Stations";
	const CuratedCountry* countries = Radio_getCuratedCountries();
	int country_count = Radio_getCuratedCountryCount();
	for (int i = 0; i < country_count; i++) {
		if (strcmp(countries[i].code, country_code) == 0) {
			country_name = countries[i].name;
			break;
		}
	}

	UI_renderMenuBar(screen, country_name);

	// Get stations for selected country
	int station_count = 0;
	const CuratedStation* stations = Radio_getCuratedStations(country_code, &station_count);

	// Use common list layout calculation
	ListLayout layout = UI_calcListLayout(screen);
	adjust_list_scroll(add_station_selected, add_station_scroll, layout.items_per_page);

	// Determine if the currently selected station is already added
	bool selected_exists = false;
	if (sorted_count > 0 && add_station_selected < sorted_count) {
		int sel_actual = sorted_indices[add_station_selected];
		if (sel_actual < station_count) {
			selected_exists = Radio_stationExists(stations[sel_actual].url);
		}
	}

	for (int i = 0; i < layout.items_per_page && *add_station_scroll + i < sorted_count; i++) {
		int idx = *add_station_scroll + i;
		int actual_idx = sorted_indices[idx];
		const CuratedStation* station = &stations[actual_idx];
		bool selected = (idx == add_station_selected);
		bool added = Radio_stationExists(station->url);

		int y = layout.list_y + i * layout.item_h;

		// Calculate prefix width for added indicator
		const char* prefix = added ? "[+] " : "";
		int prefix_width = 0;
		if (added) {
			int pw, ph;
			TTF_SizeUTF8(font.small, "[+]", &pw, &ph);
			prefix_width = pw + SCALE1(6);
		}

		// Render pill background and get text position
		int name_max_width = layout.max_width - prefix_width - SCALE1(60);
		int text_width = GFX_truncateText(font.medium, station->name, truncated, name_max_width, SCALE1(BUTTON_PADDING * 2));
		int pill_width = MIN(layout.max_width, prefix_width + text_width + SCALE1(BUTTON_PADDING));

		// Background pill
		SDL_Rect pill_rect = {SCALE1(PADDING), y, pill_width, layout.item_h};
		Fonts_drawListItemBg(screen, &pill_rect, selected);

		int text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
		int text_y = y + (layout.item_h - TTF_FontHeight(font.medium)) / 2;

		// Added indicator prefix
		if (added) {
			SDL_Color prefix_color = Fonts_getListTextColor(selected);
			SDL_Surface* prefix_text = TTF_RenderUTF8_Blended(font.small, "[+]", prefix_color);
			if (prefix_text) {
				SDL_BlitSurface(prefix_text, NULL, screen, &(SDL_Rect){text_x, y + (layout.item_h - prefix_text->h) / 2});
				SDL_FreeSurface(prefix_text);
			}
		}

		// Station name
		UI_renderListItemText(screen, NULL, station->name, font.medium,
							  text_x + prefix_width, text_y, name_max_width, selected);

		// Genre on right
		if (station->genre[0]) {
			SDL_Color genre_color = selected ? COLOR_GRAY : COLOR_DARK_TEXT;
			SDL_Surface* genre_text = TTF_RenderUTF8_Blended(font.tiny, station->genre, genre_color);
			if (genre_text) {
				SDL_BlitSurface(genre_text, NULL, screen, &(SDL_Rect){hw - genre_text->w - SCALE1(PADDING * 2), y + (layout.item_h - genre_text->h) / 2});
				SDL_FreeSurface(genre_text);
			}
		}
	}

	render_scroll_indicators(screen, *add_station_scroll, layout.items_per_page, sorted_count);

	// Toast notification
	UI_renderToast(screen, toast_message, toast_time);

	// Button hints - dynamic based on whether selected station is already added
	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", "A", selected_exists ? "REMOVE" : "ADD", NULL});
}

// Render help/instructions screen
void render_radio_help(SDL_Surface* screen, IndicatorType show_setting, int* help_scroll) {
	GFX_clear(screen);

	int hw = screen->w;
	int hh = screen->h;

	UI_renderMenuBar(screen, "How to Add Stations");

	// Content padding (aligned with title pill)
	int left_padding = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
	int right_padding = SCALE1(PADDING);
	int bottom_padding = SCALE1(PADDING);
	int max_content_width = hw - left_padding - right_padding;

	// Instructions text
	int content_start_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
	int line_h = SCALE1(18);
	int button_area_h = SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
	int visible_height = hh - content_start_y - button_area_h - bottom_padding;

	const char* lines[] = {
		"To add custom radio stations:",
		"",
		"1. Create or edit the file:",
		"   /.userdata/shared/music-player/radio/stations.txt",
		"",
		"2. Add one station per line:",
		"   Name|URL|Genre|Slogan",
		"",
		"Example:",
		"   My Radio|http://example.com/stream|Music|Slogan",
		"",
		"Notes:",
		"- MP3, AAC, and M3U8 formats supported",
		"- Maximum 32 stations",
		"- Slogan is optional (shown when no song info)",
		"",
		"Find more stations at: fmstream.org"};

	int num_lines = sizeof(lines) / sizeof(lines[0]);

	// Calculate total content height
	int total_content_h = 0;
	for (int i = 0; i < num_lines; i++) {
		if (lines[i][0] == '\0') {
			total_content_h += line_h / 2;
		} else {
			total_content_h += line_h;
		}
	}

	// Calculate max scroll
	int max_scroll = total_content_h - visible_height;
	if (max_scroll < 0)
		max_scroll = 0;
	if (*help_scroll > max_scroll)
		*help_scroll = max_scroll;
	if (*help_scroll < 0)
		*help_scroll = 0;

	// Render lines with scroll offset
	int text_y = content_start_y - *help_scroll;
	for (int i = 0; i < num_lines; i++) {
		int current_line_h = (lines[i][0] == '\0') ? line_h / 2 : line_h;

		// Skip lines that are above visible area
		if (text_y + current_line_h < content_start_y) {
			text_y += current_line_h;
			continue;
		}

		// Stop if we're below visible area
		if (text_y >= hh - button_area_h) {
			break;
		}

		if (lines[i][0] == '\0') {
			text_y += line_h / 2;
			continue;
		}

		SDL_Color color = COLOR_WHITE;
		TTF_Font* use_font = font.small;

		// Highlight special lines
		if (strstr(lines[i], "Example:") || strstr(lines[i], "Notes:")) {
			color = COLOR_GRAY;
		} else if (lines[i][0] == '-') {
			color = COLOR_GRAY;
			use_font = font.tiny;
		}

		SDL_Surface* line_text = TTF_RenderUTF8_Blended(use_font, lines[i], color);
		if (line_text) {
			SDL_BlitSurface(line_text, NULL, screen, &(SDL_Rect){left_padding, text_y});
			SDL_FreeSurface(line_text);
		}
		text_y += line_h;
	}

	// Scroll indicators
	if (max_scroll > 0) {
		int ox = (hw - SCALE1(24)) / 2;
		if (*help_scroll > 0) {
			GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, content_start_y - SCALE1(12)});
		}
		if (*help_scroll < max_scroll) {
			GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - button_area_h - bottom_padding - SCALE1(4)});
		}
	}

	// Button hints
	UI_renderButtonHintBar(screen, (char*[]){"START", "CONTROLS", "B", "BACK", NULL});
}

// === GPU STATUS AND BUFFER INDICATOR ===
// Following the same pattern as Spectrum and PlayTime in player.c:
// - Position is set during main screen render (when dirty)
// - GPU layer rendering happens independently in main loop

static int status_bar_x = 0, status_bar_y = 0, status_bar_w = 0, status_bar_h = 0;
static int status_left_x = 0, status_left_y = 0;
static bool status_position_set = false;


void RadioStatus_setPosition(int bar_x, int bar_y, int bar_w, int bar_h,
							 int left_x, int left_y) {
	status_bar_x = bar_x;
	status_bar_y = bar_y;
	status_bar_w = bar_w;
	status_bar_h = bar_h;
	status_left_x = left_x;
	status_left_y = left_y;
	status_position_set = true;
}

void RadioStatus_clear(void) {
	status_position_set = false;
	PLAT_clearLayers(LAYER_BUFFER);
	PLAT_GPU_Flip();
}

bool RadioStatus_needsRefresh(void) {
	if (!status_position_set)
		return false;
	RadioState state = Radio_getState();
	// Also refresh once when transitioning to STOPPED, to clear the layer
	static RadioState prev_state = RADIO_STATE_STOPPED;
	if (state == RADIO_STATE_STOPPED) {
		if (prev_state != RADIO_STATE_STOPPED) {
			prev_state = state;
			return true;
		}
		return false;
	}
	prev_state = state;
	return true;
}

void RadioStatus_renderGPU(void) {
	if (!status_position_set)
		return;

	RadioState state = Radio_getState();

	// When stopped, clear the status layer and reset cache
	static RadioState last_state = RADIO_STATE_STOPPED;
	static int last_bitrate = 0;
	static int last_buf_pct = -1;

	if (state == RADIO_STATE_STOPPED) {
		if (last_state != RADIO_STATE_STOPPED) {
			PLAT_clearLayers(LAYER_BUFFER);
			PLAT_GPU_Flip();
			last_state = RADIO_STATE_STOPPED;
			last_bitrate = 0;
			last_buf_pct = -1;
		}
		return;
	}

	float buffer_level = Radio_getBufferLevel();

	// Get bitrate from metadata
	const RadioMetadata* meta = Radio_getMetadata();
	int current_bitrate = meta ? meta->bitrate : 0;

	// Skip expensive surface recreation if nothing changed
	int buf_pct = (int)(buffer_level * 100);
	if (state == last_state && current_bitrate == last_bitrate && buf_pct == last_buf_pct) {
		return;
	}
	last_state = state;
	last_bitrate = current_bitrate;
	last_buf_pct = buf_pct;

	// Get status text
	// Show "buffering" only during initial connect or actual rebuffer (low buffer).
	// Once buffer is healthy, show "streaming" even if state is still BUFFERING.
	const char* status_text = "";
	switch (state) {
	case RADIO_STATE_CONNECTING:
		status_text = "connecting";
		break;
	case RADIO_STATE_BUFFERING:
		status_text = (buffer_level < 0.5f) ? "buffering" : "streaming";
		break;
	case RADIO_STATE_PLAYING:
		status_text = "streaming";
		break;
	case RADIO_STATE_ERROR:
		status_text = "error";
		break;
	default:
		break;
	}

	// Prepare bitrate string
	char bitrate_str[32] = "";
	if (current_bitrate > 0) {
		snprintf(bitrate_str, sizeof(bitrate_str), "%d kbps", current_bitrate);
	}

	// Measure all elements
	TTF_Font* bitrate_font = font.small;
	TTF_Font* status_font = font.tiny;

	int bitrate_w = 0, bitrate_h = 0;
	if (bitrate_str[0]) {
		TTF_SizeUTF8(bitrate_font, bitrate_str, &bitrate_w, &bitrate_h);
	}

	int status_w = 0, status_h = 0;
	if (status_text[0]) {
		TTF_SizeUTF8(status_font, status_text, &status_w, &status_h);
	}

	// Layout: [bitrate] [status] ----space---- [buffer bar]
	int gap = SCALE1(6);
	int left_x = status_left_x;
	int right_x = status_bar_x + status_bar_w;

	// Calculate line height for vertical centering
	int line_h = bitrate_h;
	if (status_h > line_h)
		line_h = status_h;
	if (status_bar_h > line_h)
		line_h = status_bar_h;
	int base_y = status_left_y;

	// Surface covers from left edge to right edge
	int surface_w = right_x - left_x;
	int surface_h = line_h;

	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0,
														  surface_w, surface_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surface)
		return;

	SDL_FillRect(surface, NULL, 0); // Transparent background

	int x_offset = 0;

	// Draw bitrate on left (white)
	if (bitrate_str[0]) {
		SDL_Color color = {255, 255, 255, 255};
		SDL_Surface* text_surf = TTF_RenderUTF8_Blended(bitrate_font, bitrate_str, color);
		if (text_surf) {
			int y_pos = (line_h - bitrate_h) / 2;
			SDL_Rect dst = {x_offset, y_pos, 0, 0};
			SDL_BlitSurface(text_surf, NULL, surface, &dst);
			SDL_FreeSurface(text_surf);
		}
		x_offset += bitrate_w + gap;
	}

	// Draw status text next to bitrate (gray)
	if (status_text[0]) {
		SDL_Color color = {128, 128, 128, 255};
		SDL_Surface* text_surf = TTF_RenderUTF8_Blended(status_font, status_text, color);
		if (text_surf) {
			int y_pos = (line_h - status_h) / 2;
			SDL_Rect dst = {x_offset, y_pos, 0, 0};
			SDL_BlitSurface(text_surf, NULL, surface, &dst);
			SDL_FreeSurface(text_surf);
		}
	}

	// Draw buffer bar on right side
	int bar_x_in_surface = surface_w - status_bar_w;
	int bar_y_pos = (line_h - status_bar_h) / 2;

	// Buffer bar background (dark gray)
	SDL_Rect bar_bg = {bar_x_in_surface, bar_y_pos, status_bar_w, status_bar_h};
	SDL_FillRect(surface, &bar_bg, SDL_MapRGBA(surface->format, 60, 60, 60, 255));

	// Buffer fill (white)
	int fill_w = (int)(status_bar_w * buffer_level);
	if (fill_w > 0) {
		SDL_Rect bar_fill = {bar_x_in_surface, bar_y_pos, fill_w, status_bar_h};
		SDL_FillRect(surface, &bar_fill, SDL_MapRGBA(surface->format, 255, 255, 255, 255));
	}

	// Render to GPU layer
	PLAT_clearLayers(LAYER_BUFFER);
	PLAT_drawOnLayer(surface, left_x, base_y, surface_w, surface_h, 1.0f, false, LAYER_BUFFER);
	SDL_FreeSurface(surface);

	PLAT_GPU_Flip();
}
