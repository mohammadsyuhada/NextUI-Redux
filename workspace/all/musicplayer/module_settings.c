#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "module_settings.h"
#include "settings.h"
#include "downloader.h"
#include "ytdlp_updater.h"
#include "ui_components.h"
#include "ui_settings.h"
#include "wifi.h"
#include "album_art.h"
#include "lyrics.h"
#include <stdbool.h>

// Internal states
typedef enum {
	SETTINGS_STATE_MENU,
	SETTINGS_STATE_CLEAR_CACHE_CONFIRM,
	SETTINGS_STATE_CLEAR_LYRICS_CONFIRM,
	SETTINGS_STATE_UPDATING_YTDLP
} SettingsState;

// Settings menu items
#define SETTINGS_ITEM_SCREEN_OFF 0
#define SETTINGS_ITEM_BASS_FILTER 1
#define SETTINGS_ITEM_SOFT_LIMITER 2
#define SETTINGS_ITEM_CLEAR_CACHE 3
#define SETTINGS_ITEM_CLEAR_LYRICS 4
#define SETTINGS_ITEM_UPDATE_YTDLP 5
#define SETTINGS_ITEM_COUNT 6

// Internal app state constants for controls help
// These match the pattern used in ui_main.c
#define SETTINGS_INTERNAL_MENU 40

ModuleExitReason SettingsModule_run(SDL_Surface* screen) {
	SettingsState state = SETTINGS_STATE_MENU;
	int menu_selected = 0;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (1) {
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
			// Left/Right for cyclable settings
			else if (PAD_justPressed(BTN_LEFT)) {
				if (menu_selected == SETTINGS_ITEM_SCREEN_OFF) {
					Settings_cycleScreenOffPrev();
					dirty = 1;
				} else if (menu_selected == SETTINGS_ITEM_BASS_FILTER) {
					Settings_cycleBassFilterPrev();
					dirty = 1;
				} else if (menu_selected == SETTINGS_ITEM_SOFT_LIMITER) {
					Settings_cycleSoftLimiterPrev();
					dirty = 1;
				}
			} else if (PAD_justPressed(BTN_RIGHT)) {
				if (menu_selected == SETTINGS_ITEM_SCREEN_OFF) {
					Settings_cycleScreenOffNext();
					dirty = 1;
				} else if (menu_selected == SETTINGS_ITEM_BASS_FILTER) {
					Settings_cycleBassFilterNext();
					dirty = 1;
				} else if (menu_selected == SETTINGS_ITEM_SOFT_LIMITER) {
					Settings_cycleSoftLimiterNext();
					dirty = 1;
				}
			} else if (PAD_justPressed(BTN_A)) {
				switch (menu_selected) {
				case SETTINGS_ITEM_SCREEN_OFF:
					// A also cycles the value (convenience)
					Settings_cycleScreenOffNext();
					dirty = 1;
					break;
				case SETTINGS_ITEM_BASS_FILTER:
					Settings_cycleBassFilterNext();
					dirty = 1;
					break;
				case SETTINGS_ITEM_SOFT_LIMITER:
					Settings_cycleSoftLimiterNext();
					dirty = 1;
					break;
				case SETTINGS_ITEM_CLEAR_CACHE:
					state = SETTINGS_STATE_CLEAR_CACHE_CONFIRM;
					dirty = 1;
					break;
				case SETTINGS_ITEM_CLEAR_LYRICS:
					state = SETTINGS_STATE_CLEAR_LYRICS_CONFIRM;
					dirty = 1;
					break;
				case SETTINGS_ITEM_UPDATE_YTDLP:
					if (Downloader_init() == 0 && Wifi_ensureConnected(screen, show_setting)) {
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

		case SETTINGS_STATE_CLEAR_CACHE_CONFIRM:
			if (PAD_justPressed(BTN_A)) {
				// Confirm - clear the cache
				album_art_clear_disk_cache();
				state = SETTINGS_STATE_MENU;
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				// Cancel
				state = SETTINGS_STATE_MENU;
				dirty = 1;
			}
			break;

		case SETTINGS_STATE_CLEAR_LYRICS_CONFIRM:
			if (PAD_justPressed(BTN_A)) {
				Lyrics_clearCache();
				state = SETTINGS_STATE_MENU;
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				state = SETTINGS_STATE_MENU;
				dirty = 1;
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
			case SETTINGS_STATE_CLEAR_CACHE_CONFIRM:
				render_settings_menu(screen, show_setting, menu_selected);
				UI_renderConfirmDialog(screen, "Clear album art cache?", NULL);
				break;
			case SETTINGS_STATE_CLEAR_LYRICS_CONFIRM:
				render_settings_menu(screen, show_setting, menu_selected);
				UI_renderConfirmDialog(screen, "Clear lyrics cache?", NULL);
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
