#include <stdio.h>
#include <string.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "ui_toast.h"
#include "module_library.h"
#include "module_player.h"
#include "module_playlist.h"
#include "module_downloader.h"
#include "ui_list.h"

// Library submenu items
#define LIBRARY_FILES 0
#define LIBRARY_PLAYLISTS 1
#define LIBRARY_DOWNLOADER 2
#define LIBRARY_ITEM_COUNT 3

// Help state for controls dialog
#define LIBRARY_MENU_HELP_STATE 55

static const char* library_items[] = {"Files", "Playlists", "Downloader"};

// Toast state
static char library_toast_message[128] = "";
static uint32_t library_toast_time = 0;

static void render_library_menu(SDL_Surface* screen, IndicatorType show_setting, int menu_selected) {
	SimpleMenuConfig config = {
		.title = "Library",
		.items = library_items,
		.item_count = LIBRARY_ITEM_COUNT,
		.btn_b_label = "BACK",
		.get_label = NULL,
		.render_badge = NULL,
		.get_icon = NULL};
	UI_renderSimpleMenu(screen, menu_selected, &config);
	UI_renderToast(screen, library_toast_message, library_toast_time);
}

void LibraryModule_setToast(const char* message) {
	snprintf(library_toast_message, sizeof(library_toast_message), "%s", message);
	library_toast_time = SDL_GetTicks();
}

ModuleExitReason LibraryModule_run(SDL_Surface* screen) {
	int menu_selected = 0;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (1) {
		GFX_startFrame();
		PAD_poll();

		// Handle global input
		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, LIBRARY_MENU_HELP_STATE);
		if (global.should_quit) {
			return MODULE_EXIT_QUIT;
		}
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}

		// Menu navigation
		if (PAD_justRepeated(BTN_UP)) {
			menu_selected = (menu_selected > 0) ? menu_selected - 1 : LIBRARY_ITEM_COUNT - 1;
			dirty = 1;
		} else if (PAD_justRepeated(BTN_DOWN)) {
			menu_selected = (menu_selected < LIBRARY_ITEM_COUNT - 1) ? menu_selected + 1 : 0;
			dirty = 1;
		} else if (PAD_justPressed(BTN_A)) {
			ModuleExitReason reason = MODULE_EXIT_TO_MENU;

			switch (menu_selected) {
			case LIBRARY_FILES:
				reason = PlayerModule_run(screen);
				break;
			case LIBRARY_PLAYLISTS:
				reason = PlaylistModule_run(screen);
				break;
			case LIBRARY_DOWNLOADER:
				reason = DownloaderModule_run(screen);
				break;
			}

			if (reason == MODULE_EXIT_QUIT) {
				return MODULE_EXIT_QUIT;
			}

			// Sub-module returned to library menu
			dirty = 1;
		} else if (PAD_justPressed(BTN_B)) {
			return MODULE_EXIT_TO_MENU;
		}

		// Handle power management
		ModuleCommon_PWR_update(&dirty, &show_setting);

		// Render
		if (dirty) {
			render_library_menu(screen, show_setting, menu_selected);

			GFX_flip(screen);
			dirty = 0;

			ModuleCommon_tickToast(library_toast_message, library_toast_time, &dirty);
		} else {
			GFX_sync();
		}
	}
}
