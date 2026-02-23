#include <stdio.h>
#include <string.h>

#include "api.h"
#include "ui_components.h"
#include "ui_settings.h"
#include "ui_list.h"
#include "settings.h"
#include "album_art.h"
#include "lyrics.h"

// Settings menu items
#define SETTINGS_ITEM_SCREEN_OFF 0
#define SETTINGS_ITEM_BASS_FILTER 1
#define SETTINGS_ITEM_SOFT_LIMITER 2
#define SETTINGS_ITEM_CLEAR_CACHE 3
#define SETTINGS_ITEM_CLEAR_LYRICS 4
#define SETTINGS_ITEM_UPDATE_YTDLP 5
#define SETTINGS_ITEM_COUNT 6

// Format cache size as human-readable string
static void format_cache_size(long bytes, char* buf, int buf_size) {
	if (bytes >= 1024 * 1024) {
		snprintf(buf, buf_size, "%.1f MB", bytes / (1024.0 * 1024.0));
	} else if (bytes >= 1024) {
		snprintf(buf, buf_size, "%.1f KB", bytes / 1024.0);
	} else {
		snprintf(buf, buf_size, "%ld B", bytes);
	}
}

void render_settings_menu(SDL_Surface* screen, IndicatorType show_setting, int menu_selected) {
	GFX_clear(screen);

	UI_renderMenuBar(screen, "Settings");
	ListLayout layout = UI_calcListLayout(screen);

	// Build dynamic cache labels
	static char cache_label[128];
	long cache_size = album_art_get_cache_size();
	char size_str[32];
	format_cache_size(cache_size, size_str, sizeof(size_str));
	snprintf(cache_label, sizeof(cache_label), "Clear Album Art (%s)", size_str);

	static char lyrics_label[128];
	long lyrics_size = Lyrics_getCacheSize();
	char lyrics_size_str[32];
	format_cache_size(lyrics_size, lyrics_size_str, sizeof(lyrics_size_str));
	snprintf(lyrics_label, sizeof(lyrics_label), "Clear Lyrics (%s)", lyrics_size_str);

	UISettingsItem items[] = {
		{.label = "Auto Screen Off", .value = Settings_getScreenOffDisplayStr(), .swatch = -1, .cycleable = 1, .desc = "Turn off screen while music is playing"},
		{.label = "Bass Filter", .value = Settings_getBassFilterDisplayStr(), .swatch = -1, .cycleable = 1, .desc = "High-pass filter to reduce speaker distortion"},
		{.label = "Soft Limiter", .value = Settings_getSoftLimiterDisplayStr(), .swatch = -1, .cycleable = 1, .desc = "Limit volume peaks to prevent clipping"},
		{.label = cache_label, .swatch = -1, .desc = "Delete cached album art images"},
		{.label = lyrics_label, .swatch = -1, .desc = "Delete cached lyrics files"},
		{.label = "Update yt-dlp", .swatch = -1, .desc = "Download the latest version of yt-dlp"},
	};

	static int scroll = 0;
	UI_renderSettingsPage(screen, &layout, items, SETTINGS_ITEM_COUNT,
						  menu_selected, &scroll, NULL);

	bool is_cyclable = (menu_selected == SETTINGS_ITEM_SCREEN_OFF ||
						menu_selected == SETTINGS_ITEM_BASS_FILTER ||
						menu_selected == SETTINGS_ITEM_SOFT_LIMITER);

	UI_renderButtonHintBar(screen, (char*[]){
									   "START", "CONTROLS",
									   "B", "BACK",
									   is_cyclable ? "LEFT/RIGHT" : "A",
									   is_cyclable ? "CHANGE" : "OPEN",
									   NULL});
}
