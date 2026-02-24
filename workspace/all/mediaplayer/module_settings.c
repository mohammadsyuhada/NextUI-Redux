#include <stdio.h>
#include <string.h>
#include "api.h"
#include "module_common.h"
#include "module_settings.h"
#include "ui_settings.h"
#include "ytdlp_updater.h"
#include "wifi.h"

// Internal states
typedef enum {
	SETTINGS_STATE_MENU,
	SETTINGS_STATE_UPDATING_YTDLP
} SettingsState;

// Settings menu items
#define SETTINGS_ITEM_UPDATE_YTDLP 0
#define SETTINGS_ITEM_COUNT 1

// Internal app state constants for controls help
// These match the pattern used in ui_main.c
#define SETTINGS_INTERNAL_MENU 41

ModuleExitReason SettingsModule_run(SDL_Surface* screen) {
	SettingsState state = SETTINGS_STATE_MENU;
	int menu_selected = 0;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (1) {
		GFX_startFrame();
		PAD_poll();

		// Handle global input first
		int app_state = SETTINGS_INTERNAL_MENU;
		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, app_state);
		if (global.should_quit) {
			return MODULE_EXIT_QUIT;
		}
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}

		// State-specific handling
		switch (state) {
		case SETTINGS_STATE_MENU:
			// Navigation
			if (PAD_navigateMenu(&menu_selected, SETTINGS_ITEM_COUNT))
				dirty = 1;
			else if (PAD_justPressed(BTN_A)) {
				switch (menu_selected) {
				case SETTINGS_ITEM_UPDATE_YTDLP:
					if (Wifi_ensureConnected(screen, show_setting)) {
						YtdlpUpdater_startUpdate();
						state = SETTINGS_STATE_UPDATING_YTDLP;
					}
					dirty = 1;
					break;
				}
			}
			// B button - back to main menu
			else if (PAD_justPressed(BTN_B)) {
				return MODULE_EXIT_TO_MENU;
			}
			break;

		case SETTINGS_STATE_UPDATING_YTDLP: {
			const YtdlpUpdateStatus* ytdlp_status = YtdlpUpdater_getUpdateStatus();

			if (PAD_justPressed(BTN_B)) {
				if (ytdlp_status->updating) {
					YtdlpUpdater_cancelUpdate();
				}
				state = SETTINGS_STATE_MENU;
			}

			// Always repaint while on the updating screen
			// (need to show final state: success, error, or already-up-to-date)
			dirty = 1;

			break;
		}
		}

		// Handle power management
		ModuleCommon_PWR_update(&dirty, &show_setting);

		// Render
		if (dirty) {
			switch (state) {
			case SETTINGS_STATE_MENU:
				render_settings_menu(screen, show_setting, menu_selected);
				break;
			case SETTINGS_STATE_UPDATING_YTDLP:
				render_ytdlp_updating(screen, show_setting);
				break;
			}

			GFX_flip(screen);
			dirty = 0;
		} else {
			GFX_sync();
		}
	}
}
