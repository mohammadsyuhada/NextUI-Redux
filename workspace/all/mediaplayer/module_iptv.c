#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vp_defines.h"
#include "api.h"
#include "module_common.h"
#include "module_iptv.h"
#include "iptv.h"
#include "iptv_curated.h"
#include "wifi.h"
#include "ffplay_engine.h"
#include "ui_components.h"
#include "ui_iptv.h"
#include "ui_main.h"
#include "ui_icons.h"
#include "ui_utils.h"

// Module states
typedef enum {
	IPTV_STATE_USER_CHANNELS,	  // Main screen: user's saved channels
	IPTV_STATE_CURATED_COUNTRIES, // Browse curated countries
	IPTV_STATE_CURATED_CHANNELS	  // Browse curated channels in a country
} IPTVModuleState;

static ScrollTextState iptv_scroll = {0};

// Curated browse state
static int curated_country_selected = 0;
static int curated_country_scroll = 0;
static int curated_channel_selected = 0;
static int curated_channel_scroll = 0;
static const char* curated_selected_country_code = NULL;
static char curated_toast_message[128] = "";
static uint32_t curated_toast_time = 0;

// Confirmation dialog state
static bool show_confirm = false;
static int confirm_action_type = 0; // 0 = delete from main list, 1 = remove from browse
static int confirm_target_index = -1;
static char confirm_channel_name[IPTV_MAX_NAME] = "";
static char confirm_channel_url[IPTV_MAX_URL] = "";

// Sorted channel index mapping for alphabetical display
static int sorted_channel_indices[256];
static int sorted_channel_count = 0;

static void build_sorted_channel_indices(const char* country_code) {
	int sc = 0;
	const CuratedTVChannel* cs = IPTV_curated_get_channels(country_code, &sc);
	sorted_channel_count = (sc < 256) ? sc : 256;
	for (int i = 0; i < sorted_channel_count; i++)
		sorted_channel_indices[i] = i;
	// Insertion sort by name
	for (int i = 1; i < sorted_channel_count; i++) {
		int key = sorted_channel_indices[i];
		int j = i - 1;
		while (j >= 0 && strcasecmp(cs[sorted_channel_indices[j]].name, cs[key].name) > 0) {
			sorted_channel_indices[j + 1] = sorted_channel_indices[j];
			j--;
		}
		sorted_channel_indices[j + 1] = key;
	}
}

ModuleExitReason IPTVModule_run(SDL_Surface* screen) {
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;
	int ch_selected = 0, ch_scroll = 0;
	IPTVModuleState state = IPTV_STATE_USER_CHANNELS;

	memset(&iptv_scroll, 0, sizeof(iptv_scroll));
	show_confirm = false;

	while (1) {
		PAD_poll();

		// Handle confirmation dialog
		if (show_confirm) {
			if (PAD_justPressed(BTN_A)) {
				if (confirm_action_type == 0) {
					// Delete from main list
					IPTV_removeUserChannel(confirm_target_index);
					int user_count = IPTV_getUserChannelCount();
					if (ch_selected >= user_count && ch_selected > 0) {
						ch_selected--;
					}
					memset(&iptv_scroll, 0, sizeof(iptv_scroll));
				} else if (confirm_action_type == 1) {
					// Remove from curated browse
					IPTV_removeUserChannelByUrl(confirm_channel_url);
					snprintf(curated_toast_message, sizeof(curated_toast_message), "Removed: %s", confirm_channel_name);
					curated_toast_time = SDL_GetTicks();
				}
				show_confirm = false;
				dirty = 1;
				GFX_sync();
				continue;
			} else if (PAD_justPressed(BTN_B)) {
				show_confirm = false;
				dirty = 1;
				GFX_sync();
				continue;
			}
			// Render confirmation dialog (covers entire screen)
			render_confirmation_dialog(screen, confirm_channel_name, "Remove Channel?");
			GFX_flip(screen);
			GFX_sync();
			continue;
		}

		// Handle curated countries browsing
		if (state == IPTV_STATE_CURATED_COUNTRIES) {
			GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, STATE_IPTV_CURATED_COUNTRIES);
			if (global.should_quit)
				return MODULE_EXIT_QUIT;
			if (global.input_consumed) {
				if (global.dirty)
					dirty = 1;
				GFX_sync();
				continue;
			}

			int country_count = IPTV_curated_get_country_count();

			if (PAD_justRepeated(BTN_UP) && country_count > 0) {
				curated_country_selected = (curated_country_selected > 0) ? curated_country_selected - 1 : country_count - 1;
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN) && country_count > 0) {
				curated_country_selected = (curated_country_selected < country_count - 1) ? curated_country_selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A) && country_count > 0) {
				const CuratedTVCountry* countries = IPTV_curated_get_countries();
				curated_selected_country_code = countries[curated_country_selected].code;
				curated_channel_selected = 0;
				curated_channel_scroll = 0;
				build_sorted_channel_indices(curated_selected_country_code);
				state = IPTV_STATE_CURATED_CHANNELS;
				dirty = 1;
				continue;
			} else if (PAD_justPressed(BTN_B)) {
				state = IPTV_STATE_USER_CHANNELS;
				ch_selected = 0;
				ch_scroll = 0;
				memset(&iptv_scroll, 0, sizeof(iptv_scroll));
				dirty = 1;
				continue;
			}

			ModuleCommon_PWR_update(&dirty, &show_setting);
			if (dirty) {
				render_iptv_curated_countries(screen, show_setting, curated_country_selected, &curated_country_scroll);
				GFX_flip(screen);
				dirty = 0;
			} else {
				GFX_sync();
			}
			continue;
		}

		// Handle curated channels browsing
		if (state == IPTV_STATE_CURATED_CHANNELS) {
			GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, STATE_IPTV_CURATED_CHANNELS);
			if (global.should_quit)
				return MODULE_EXIT_QUIT;
			if (global.input_consumed) {
				if (global.dirty)
					dirty = 1;
				GFX_sync();
				continue;
			}

			int channel_count = 0;
			const CuratedTVChannel* channels = IPTV_curated_get_channels(curated_selected_country_code, &channel_count);

			if (PAD_justRepeated(BTN_UP) && sorted_channel_count > 0) {
				curated_channel_selected = (curated_channel_selected > 0) ? curated_channel_selected - 1 : sorted_channel_count - 1;
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN) && sorted_channel_count > 0) {
				curated_channel_selected = (curated_channel_selected < sorted_channel_count - 1) ? curated_channel_selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A) && sorted_channel_count > 0) {
				int actual_idx = sorted_channel_indices[curated_channel_selected];
				const CuratedTVChannel* channel = &channels[actual_idx];
				if (IPTV_userChannelExists(channel->url)) {
					// Already added - confirm removal
					strncpy(confirm_channel_name, channel->name, IPTV_MAX_NAME - 1);
					confirm_channel_name[IPTV_MAX_NAME - 1] = '\0';
					strncpy(confirm_channel_url, channel->url, IPTV_MAX_URL - 1);
					confirm_channel_url[IPTV_MAX_URL - 1] = '\0';
					confirm_action_type = 1;
					show_confirm = true;
					dirty = 1;
				} else {
					if (IPTV_addUserChannel(channel->name, channel->url, channel->category, channel->logo, channel->decryption_key) >= 0) {
						snprintf(curated_toast_message, sizeof(curated_toast_message), "Added: %s", channel->name);
						curated_toast_time = SDL_GetTicks();
					} else {
						snprintf(curated_toast_message, sizeof(curated_toast_message), "Maximum %d channels reached", IPTV_MAX_USER_CHANNELS);
						curated_toast_time = SDL_GetTicks();
					}
				}
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				curated_toast_message[0] = '\0';
				UI_clearToast();
				state = IPTV_STATE_CURATED_COUNTRIES;
				dirty = 1;
				continue;
			}

			ModuleCommon_PWR_update(&dirty, &show_setting);
			if (dirty) {
				render_iptv_curated_channels(screen, show_setting, curated_selected_country_code,
											 curated_channel_selected, &curated_channel_scroll,
											 sorted_channel_indices, sorted_channel_count,
											 curated_toast_message, curated_toast_time);
				GFX_flip(screen);
				dirty = 0;

				ModuleCommon_tickToast(curated_toast_message, curated_toast_time, &dirty);
			} else {
				GFX_sync();
			}
			continue;
		}

		// IPTV_STATE_USER_CHANNELS (main screen)
		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, STATE_IPTV_LIST);
		if (global.should_quit)
			return MODULE_EXIT_QUIT;
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}

		int user_count = IPTV_getUserChannelCount();

		if (PAD_justPressed(BTN_B)) {
			GFX_clearLayers(LAYER_SCROLLTEXT);
			return MODULE_EXIT_TO_MENU;
		} else if (PAD_justPressed(BTN_Y)) {
			// Open curated channel browser
			curated_country_selected = 0;
			curated_country_scroll = 0;
			state = IPTV_STATE_CURATED_COUNTRIES;
			dirty = 1;
			continue;
		} else if (PAD_justPressed(BTN_X) && user_count > 0) {
			// Confirm removal
			const IPTVChannel* channels = IPTV_getUserChannels();
			strncpy(confirm_channel_name, channels[ch_selected].name, IPTV_MAX_NAME - 1);
			confirm_channel_name[IPTV_MAX_NAME - 1] = '\0';
			confirm_target_index = ch_selected;
			confirm_action_type = 0;
			show_confirm = true;
			dirty = 1;
		} else if (user_count > 0) {
			if (PAD_justRepeated(BTN_UP)) {
				ch_selected = (ch_selected > 0) ? ch_selected - 1 : user_count - 1;
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN)) {
				ch_selected = (ch_selected < user_count - 1) ? ch_selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				const IPTVChannel* channels = IPTV_getUserChannels();
				const IPTVChannel* ch = &channels[ch_selected];

				// Ensure WiFi and play stream
				Wifi_ensureConnected(screen, show_setting);

				FfplayConfig config;
				memset(&config, 0, sizeof(config));
				config.source = FFPLAY_SOURCE_STREAM;
				config.is_stream = true;
				config.screen_width = screen->w;
				strncpy(config.path, ch->url, sizeof(config.path) - 1);
				strncpy(config.title, ch->name, sizeof(config.title) - 1);
				if (ch->decryption_key[0])
					strncpy(config.decryption_key, ch->decryption_key, sizeof(config.decryption_key) - 1);

				ModuleCommon_setAutosleepDisabled(true);
				FfplayEngine_play(&config);

				Icons_init();
				memset(&iptv_scroll, 0, sizeof(iptv_scroll));
				dirty = 1;
			}
		}

		if (ScrollText_isScrolling(&iptv_scroll))
			ScrollText_animateOnly(&iptv_scroll);
		if (ScrollText_needsRender(&iptv_scroll))
			dirty = 1;

		ModuleCommon_PWR_update(&dirty, &show_setting);
		if (dirty) {
			if (user_count > 0) {
				render_iptv_user_channels(screen, show_setting,
										  ch_selected, ch_scroll, &iptv_scroll);
			} else {
				render_iptv_empty(screen, show_setting);
			}
			GFX_flip(screen);
			dirty = 0;
		} else {
			GFX_sync();
		}
	}
}
