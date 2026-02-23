#include <stdio.h>
#include <string.h>

#include "vp_defines.h"
#include "api.h"
#include "module_common.h"
#include "module_menu.h"
#include "ui_main.h"

// Toast message state
static char menu_toast_message[128] = "";
static uint32_t menu_toast_time = 0;

// Menu items: Local, YouTube, IPTV, Settings
#define MENU_ITEM_COUNT 4

// Map visual index -> logical menu item
static int get_menu_item_id(int visual_index) {
	switch (visual_index) {
	case 0:
		return MENU_LOCAL;
	case 1:
		return MENU_YOUTUBE;
	case 2:
		return MENU_IPTV;
	case 3:
		return MENU_SETTINGS;
	default:
		return MENU_LOCAL;
	}
}

int MenuModule_run(SDL_Surface* screen) {
	int menu_selected = 0;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;
	int total_items = MENU_ITEM_COUNT;

	while (1) {
		PAD_poll();

		// Handle global input first (START dialogs, power)
		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, STATE_MENU);
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
			menu_selected = (menu_selected > 0) ? menu_selected - 1 : total_items - 1;
			GFX_clearLayers(LAYER_SCROLLTEXT);
			dirty = 1;
		} else if (PAD_justRepeated(BTN_DOWN)) {
			menu_selected = (menu_selected < total_items - 1) ? menu_selected + 1 : 0;
			GFX_clearLayers(LAYER_SCROLLTEXT);
			dirty = 1;
		} else if (PAD_justPressed(BTN_A)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			return get_menu_item_id(menu_selected);
		} else if (PAD_justPressed(BTN_B)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			return MENU_QUIT;
		}

		// Handle power management
		ModuleCommon_PWR_update(&dirty, &show_setting);

		// Render
		if (dirty) {
			render_menu(screen, show_setting, menu_selected,
						menu_toast_message, menu_toast_time);

			GFX_flip(screen);
			dirty = 0;

			// Keep refreshing while toast is visible
			ModuleCommon_tickToast(menu_toast_message, menu_toast_time, &dirty);
		} else {
			GFX_sync();
		}
	}
}

// Set toast message (called by modules that return to menu with a message)
void MenuModule_setToast(const char* message) {
	snprintf(menu_toast_message, sizeof(menu_toast_message), "%s", message);
	menu_toast_time = SDL_GetTicks();
}
