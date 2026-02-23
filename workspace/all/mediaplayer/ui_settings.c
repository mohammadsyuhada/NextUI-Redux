#include <stdio.h>
#include <string.h>

#include "api.h"
#include "ui_components.h"
#include "ui_settings.h"
#include "ui_list.h"

// Settings menu items
#define SETTINGS_ITEM_UPDATE_YTDLP 5
#define SETTINGS_ITEM_COUNT 1

void render_settings_menu(SDL_Surface* screen, IndicatorType show_setting, int menu_selected) {
	GFX_clear(screen);

	UI_renderMenuBar(screen, "Settings");
	ListLayout layout = UI_calcListLayout(screen);

	UISettingsItem items[] = {
		{.label = "Update yt-dlp", .swatch = -1, .desc = "Download the latest version of yt-dlp"},
	};

	static int scroll = 0;
	UI_renderSettingsPage(screen, &layout, items, SETTINGS_ITEM_COUNT,
						  menu_selected, &scroll, NULL);

	UI_renderButtonHintBar(screen, (char*[]){
									   "START", "CONTROLS",
									   "B", "BACK",
									   "A",
									   "OPEN",
									   NULL});
}
