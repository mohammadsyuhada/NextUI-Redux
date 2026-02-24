#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "module_menu.h"
#include "ui_main.h"
#include "resume.h"
#include "background.h"

// Toast message state
static char menu_toast_message[128] = "";
static uint32_t menu_toast_time = 0;

int MenuModule_run(SDL_Surface* screen) {
	int menu_selected = 0;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (1) {
		GFX_startFrame();
		PAD_poll();

		// Handle background player updates (track advancement, resume saving)
		Background_tick();
		if (Background_isPlaying()) {
			ModuleCommon_setAutosleepDisabled(true);
		}

		// Determine first item: Now Playing (if BG active) > Resume > none
		int first_item_mode = MENU_FIRST_NONE;
		if (Background_isPlaying()) {
			first_item_mode = MENU_FIRST_NOW_PLAYING;
		} else if (Resume_isAvailable()) {
			first_item_mode = MENU_FIRST_RESUME;
		}
		bool has_first = (first_item_mode != MENU_FIRST_NONE);
		int item_count = has_first ? 5 : 4;

		// Handle global input first (volume, START dialogs, power)
		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 0);
		if (global.should_quit) {
			return MENU_QUIT;
		}
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}

		// Menu navigation
		if (PAD_justRepeated(BTN_UP)) {
			menu_selected = (menu_selected > 0) ? menu_selected - 1 : item_count - 1;
			GFX_clearLayers(LAYER_SCROLLTEXT);
			dirty = 1;
		} else if (PAD_justRepeated(BTN_DOWN)) {
			menu_selected = (menu_selected < item_count - 1) ? menu_selected + 1 : 0;
			GFX_clearLayers(LAYER_SCROLLTEXT);
			dirty = 1;
		} else if (PAD_justPressed(BTN_A)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			// Adjust selection to match MENU_* constants
			int selection = menu_selected;
			if (!has_first)
				selection += 1; // Skip first-item slot
			return selection;
		} else if (PAD_justPressed(BTN_X)) {
			if (menu_selected == 0) {
				if (first_item_mode == MENU_FIRST_NOW_PLAYING) {
					// Stop background playback
					Background_stopAll();
					GFX_clearLayers(LAYER_SCROLLTEXT);
					menu_selected = 0;
					dirty = 1;
				} else if (first_item_mode == MENU_FIRST_RESUME) {
					// Clear resume history
					Resume_clear();
					GFX_clearLayers(LAYER_SCROLLTEXT);
					menu_selected = 0;
					dirty = 1;
				}
			}
		} else if (PAD_justPressed(BTN_B)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			// Exit app from main menu
			return MENU_QUIT;
		}

		// Handle power management
		ModuleCommon_PWR_update(&dirty, &show_setting);

		// Render
		if (dirty) {
			render_menu(screen, show_setting, menu_selected,
						menu_toast_message, menu_toast_time, first_item_mode);

			GFX_flip(screen);
			dirty = 0;

			// Keep refreshing while toast is visible
			ModuleCommon_tickToast(menu_toast_message, menu_toast_time, &dirty);
		} else {
			// Software scroll needs continuous redraws
			if (menu_needs_scroll_redraw())
				dirty = 1;
			GFX_sync();
		}
	}
}

// Set toast message (called by modules that return to menu with a message)
void MenuModule_setToast(const char* message) {
	snprintf(menu_toast_message, sizeof(menu_toast_message), "%s", message);
	menu_toast_time = SDL_GetTicks();
}
