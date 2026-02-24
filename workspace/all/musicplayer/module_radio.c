#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "module_radio.h"
#include "player.h"
#include "radio.h"
#include "album_art.h"
#include "ui_components.h"
#include "ui_radio.h"
#include "ui_album_art.h"
#include "ui_main.h"
#include "ui_toast.h"
#include "wifi.h"
#include "background.h"

// Internal states
typedef enum {
	RADIO_INTERNAL_LIST,
	RADIO_INTERNAL_PLAYING,
	RADIO_INTERNAL_ADD_COUNTRY,
	RADIO_INTERNAL_ADD_STATIONS,
	RADIO_INTERNAL_HELP
} RadioInternalState;

// Module state
static int radio_selected = 0;
static int radio_scroll = 0;
static char radio_toast_message[128] = "";
static uint32_t radio_toast_time = 0;

// Add stations UI state
static int add_country_selected = 0;
static int add_country_scroll = 0;
static int add_station_selected = 0;
static int add_station_scroll = 0;
static const char* add_selected_country_code = NULL;
static int help_scroll = 0;

// Confirmation dialog state
static bool show_confirm = false;
static int confirm_action_type = 0; // 0 = delete from main list, 1 = remove from browse
static int confirm_target_index = -1;
static char confirm_station_name[RADIO_MAX_NAME] = "";
static char confirm_station_url[RADIO_MAX_URL] = "";

// Help screen back-navigation
static RadioInternalState help_return_state = RADIO_INTERNAL_ADD_COUNTRY;

// Sorted station index mapping for alphabetical display
static int sorted_station_indices[256];
static int sorted_station_count = 0;

// Screen off state
static bool screen_off = false;

// Last rendered metadata (for change detection)
static char last_rendered_artist[256] = "";
static char last_rendered_title[256] = "";
static bool last_art_was_fetching = false;

// Handle USB/Bluetooth media button events
static void handle_hid_events(void) {
	USBHIDEvent hid_event;
	while ((hid_event = Player_pollUSBHID()) != USB_HID_EVENT_NONE) {
		if (hid_event == USB_HID_EVENT_PLAY_PAUSE) {
			if (Radio_isActive()) {
				Radio_stop();
			} else {
				const char* url = Radio_getCurrentUrl();
				if (url && url[0] != '\0') {
					Radio_play(url);
				}
			}
		} else if (hid_event == USB_HID_EVENT_NEXT_TRACK || hid_event == USB_HID_EVENT_PREV_TRACK) {
			RadioStation* stations;
			int station_count = Radio_getStations(&stations);
			if (station_count > 1) {
				int current_idx = Radio_findCurrentStationIndex();
				if (current_idx < 0)
					current_idx = 0;
				int new_idx = (hid_event == USB_HID_EVENT_NEXT_TRACK)
								  ? (current_idx + 1) % station_count
								  : (current_idx - 1 + station_count) % station_count;
				Radio_stop();
				Radio_play(stations[new_idx].url);
			}
		} else {
			ModuleCommon_handleHIDVolume(hid_event);
		}
	}
}

static void build_sorted_station_indices(const char* country_code) {
	int sc = 0;
	const CuratedStation* cs = Radio_getCuratedStations(country_code, &sc);
	sorted_station_count = (sc < 256) ? sc : 256;
	for (int i = 0; i < sorted_station_count; i++)
		sorted_station_indices[i] = i;
	// Insertion sort by name (max ~50 stations per country, adequate)
	for (int i = 1; i < sorted_station_count; i++) {
		int key = sorted_station_indices[i];
		int j = i - 1;
		while (j >= 0 && strcasecmp(cs[sorted_station_indices[j]].name, cs[key].name) > 0) {
			sorted_station_indices[j + 1] = sorted_station_indices[j];
			j--;
		}
		sorted_station_indices[j + 1] = key;
	}
}

ModuleExitReason RadioModule_run(SDL_Surface* screen) {
	Radio_init();

	RadioInternalState state = RADIO_INTERNAL_LIST;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	screen_off = false;
	ModuleCommon_resetScreenOffHint();
	ModuleCommon_recordInputTime();
	radio_toast_message[0] = '\0';
	show_confirm = false;

	// Re-enter playing state if radio is playing in background
	if (Background_getActive() == BG_RADIO && Radio_isActive()) {
		Background_setActive(BG_NONE);
		ModuleCommon_setAutosleepDisabled(true);
		last_rendered_artist[0] = '\0';
		last_rendered_title[0] = '\0';
		last_art_was_fetching = false;
		state = RADIO_INTERNAL_PLAYING;
	}

	while (1) {
		GFX_startFrame();
		PAD_poll();

		// Handle confirmation dialog
		if (show_confirm) {
			if (PAD_justPressed(BTN_A)) {
				if (confirm_action_type == 0) {
					// Delete from main list
					Radio_removeStation(confirm_target_index);
					Radio_saveStations();
					RadioStation* stations;
					int station_count = Radio_getStations(&stations);
					if (radio_selected >= station_count && station_count > 0) {
						radio_selected = station_count - 1;
					} else if (station_count == 0) {
						radio_selected = 0;
					}
				} else if (confirm_action_type == 1) {
					// Remove from browse
					Radio_removeStationByUrl(confirm_station_url);
					Radio_saveStations();
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
			UI_renderConfirmDialog(screen, "Remove Station?", confirm_station_name);
			GFX_flip(screen);
			GFX_sync();
			continue;
		}

		// Handle global input (skip if screen off or hint active)
		if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
			// Map internal state to app state for controls help context
			int app_state_for_help;
			switch (state) {
			case RADIO_INTERNAL_LIST:
				app_state_for_help = 3;
				break; // STATE_RADIO_LIST
			case RADIO_INTERNAL_PLAYING:
				app_state_for_help = 4;
				break; // STATE_RADIO_PLAYING
			case RADIO_INTERNAL_ADD_COUNTRY:
				app_state_for_help = 5;
				break; // STATE_RADIO_ADD
			case RADIO_INTERNAL_ADD_STATIONS:
				app_state_for_help = 6;
				break; // STATE_RADIO_ADD_STATIONS
			case RADIO_INTERNAL_HELP:
				app_state_for_help = 7;
				break; // STATE_RADIO_HELP
			default:
				app_state_for_help = 3;
				break;
			}

			GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, app_state_for_help);
			if (global.should_quit) {
				Radio_quit();
				return MODULE_EXIT_QUIT;
			}
			if (global.input_consumed) {
				if (global.dirty)
					dirty = 1;
				GFX_sync();
				continue;
			}
		}

		// =========================================
		// RADIO LIST STATE
		// =========================================
		if (state == RADIO_INTERNAL_LIST) {
			RadioStation* stations;
			int station_count = Radio_getStations(&stations);

			if (PAD_navigateMenu(&radio_selected, station_count))
				dirty = 1;
			else if (PAD_justPressed(BTN_A) && station_count > 0) {
				if (!Wifi_ensureConnected(screen, show_setting)) {
					snprintf(radio_toast_message, sizeof(radio_toast_message), "Internet connection required");
					radio_toast_time = SDL_GetTicks();
					dirty = 1;
				} else {
					Background_stopAll();
					if (Radio_play(stations[radio_selected].url) == 0) {
						ModuleCommon_recordInputTime();
						last_rendered_artist[0] = '\0';
						last_rendered_title[0] = '\0';
						last_art_was_fetching = false;
						state = RADIO_INTERNAL_PLAYING;
						dirty = 1;
					}
				}
			} else if (PAD_justPressed(BTN_B)) {
				if (!Radio_isActive()) {
					Radio_quit();
				}
				return MODULE_EXIT_TO_MENU;
			} else if (PAD_justPressed(BTN_Y)) {
				add_country_selected = 0;
				add_country_scroll = 0;
				state = RADIO_INTERNAL_ADD_COUNTRY;
				dirty = 1;
			} else if (PAD_justPressed(BTN_X) && station_count > 0) {
				strncpy(confirm_station_name, stations[radio_selected].name, RADIO_MAX_NAME - 1);
				confirm_station_name[RADIO_MAX_NAME - 1] = '\0';
				confirm_target_index = radio_selected;
				confirm_action_type = 0;
				show_confirm = true;
				dirty = 1;
			}
		}
		// =========================================
		// RADIO PLAYING STATE
		// =========================================
		else if (state == RADIO_INTERNAL_PLAYING) {
			ModuleCommon_setAutosleepDisabled(true);

			// Handle screen off hint timeout
			if (ModuleCommon_isScreenOffHintActive()) {
				if (ModuleCommon_processScreenOffHintTimeout()) {
					screen_off = true;
					GFX_clear(screen);
					GFX_flip(screen);
				}
				Radio_update();
				GFX_sync();
				continue;
			}

			// Handle screen off mode
			if (screen_off) {
				// Wake screen with SELECT+A
				if (PAD_isPressed(BTN_SELECT) && PAD_isPressed(BTN_A)) {
					screen_off = false;
					PLAT_enableBacklight(1);
					ModuleCommon_recordInputTime();
					dirty = 1;
				}
				// Handle USB/Bluetooth media and volume buttons even with screen off
				handle_hid_events();
				ModuleCommon_handleHardwareVolume();
				Radio_update();
				GFX_sync();
				continue;
			}

			// Reset input timer on any button press
			if (PAD_anyPressed()) {
				ModuleCommon_recordInputTime();
			}

			// Station switching
			RadioStation* stations;
			int station_count = Radio_getStations(&stations);

			if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_R1)) {
				if (station_count > 1) {
					radio_selected = (radio_selected + 1) % station_count;
					Radio_stop();
					Radio_play(stations[radio_selected].url);
					dirty = 1;
				}
			} else if (PAD_justPressed(BTN_DOWN) || PAD_justPressed(BTN_L1)) {
				if (station_count > 1) {
					radio_selected = (radio_selected - 1 + station_count) % station_count;
					Radio_stop();
					Radio_play(stations[radio_selected].url);
					dirty = 1;
				}
			} else if (PAD_justPressed(BTN_B)) {
				cleanup_album_art_background();
				RadioStatus_clear();
				if (Radio_isActive()) {
					Background_setActive(BG_RADIO);
				} else {
					ModuleCommon_setAutosleepDisabled(false);
				}
				state = RADIO_INTERNAL_LIST;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				// A toggles play/pause
				if (Radio_isActive()) {
					// Playing - stop it
					Radio_stop();
					dirty = 1;
				} else {
					// Stopped - resume playing
					const char* url = Radio_getCurrentUrl();
					if (url && url[0] != '\0') {
						Radio_play(url);
						dirty = 1;
					}
				}
			} else if (PAD_tappedSelect(SDL_GetTicks())) {
				ModuleCommon_startScreenOffHint();
				GFX_clearLayers(LAYER_SCROLLTEXT);
				PLAT_clearLayers(LAYER_BUFFER);
				PLAT_GPU_Flip();
				dirty = 1;
			}

			Radio_update();

			// Check if metadata or album art changed (updated by stream thread)
			{
				const RadioMetadata* meta = Radio_getMetadata();
				bool fetching = album_art_is_fetching();
				if (strcmp(last_rendered_artist, meta->artist) != 0 ||
					strcmp(last_rendered_title, meta->title) != 0 ||
					(last_art_was_fetching && !fetching)) {
					dirty = 1;
				}
				last_art_was_fetching = fetching;
			}

			// Auto screen-off after inactivity
			if (Radio_getState() == RADIO_STATE_PLAYING && ModuleCommon_checkAutoScreenOffTimeout()) {
				GFX_clearLayers(LAYER_SCROLLTEXT);
				PLAT_clearLayers(LAYER_BUFFER);
				PLAT_GPU_Flip();
				dirty = 1;
			}

			// Animate radio GPU layer
			if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
				if (RadioStatus_needsRefresh()) {
					RadioStatus_renderGPU();
				}
			}
		}
		// =========================================
		// ADD COUNTRY STATE
		// =========================================
		else if (state == RADIO_INTERNAL_ADD_COUNTRY) {
			int country_count = Radio_getCuratedCountryCount();

			if (PAD_navigateMenu(&add_country_selected, country_count))
				dirty = 1;
			else if (PAD_justPressed(BTN_A) && country_count > 0) {
				const CuratedCountry* countries = Radio_getCuratedCountries();
				add_selected_country_code = countries[add_country_selected].code;
				add_station_selected = 0;
				add_station_scroll = 0;
				build_sorted_station_indices(add_selected_country_code);
				state = RADIO_INTERNAL_ADD_STATIONS;
				dirty = 1;
			} else if (PAD_justPressed(BTN_Y)) {
				help_return_state = RADIO_INTERNAL_ADD_COUNTRY;
				help_scroll = 0;
				state = RADIO_INTERNAL_HELP;
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				state = RADIO_INTERNAL_LIST;
				dirty = 1;
			}
		}
		// =========================================
		// ADD STATIONS STATE
		// =========================================
		else if (state == RADIO_INTERNAL_ADD_STATIONS) {
			int station_count = 0;
			const CuratedStation* stations = Radio_getCuratedStations(add_selected_country_code, &station_count);

			if (PAD_navigateMenu(&add_station_selected, sorted_station_count))
				dirty = 1;
			else if (PAD_justPressed(BTN_A) && sorted_station_count > 0) {
				int actual_idx = sorted_station_indices[add_station_selected];
				const CuratedStation* station = &stations[actual_idx];
				if (Radio_stationExists(station->url)) {
					// Already subscribed - confirm removal
					strncpy(confirm_station_name, station->name, RADIO_MAX_NAME - 1);
					confirm_station_name[RADIO_MAX_NAME - 1] = '\0';
					strncpy(confirm_station_url, station->url, RADIO_MAX_URL - 1);
					confirm_station_url[RADIO_MAX_URL - 1] = '\0';
					confirm_action_type = 1;
					show_confirm = true;
					dirty = 1;
				} else {
					// Not subscribed - add instantly
					if (Radio_addStation(station->name, station->url, station->genre, station->slogan) >= 0) {
						Radio_saveStations();
						snprintf(radio_toast_message, sizeof(radio_toast_message), "Added: %s", station->name);
						radio_toast_time = SDL_GetTicks();
					} else {
						snprintf(radio_toast_message, sizeof(radio_toast_message), "Maximum 32 stations reached");
						radio_toast_time = SDL_GetTicks();
					}
					dirty = 1;
				}
			} else if (PAD_justPressed(BTN_Y)) {
				help_return_state = RADIO_INTERNAL_ADD_STATIONS;
				help_scroll = 0;
				state = RADIO_INTERNAL_HELP;
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				radio_toast_message[0] = '\0';
				UI_clearToast();
				state = RADIO_INTERNAL_ADD_COUNTRY;
				dirty = 1;
			}
		}
		// =========================================
		// HELP STATE
		// =========================================
		else if (state == RADIO_INTERNAL_HELP) {
			int scroll_step = SCALE1(18);

			if (PAD_justRepeated(BTN_UP)) {
				if (help_scroll > 0) {
					help_scroll -= scroll_step;
					if (help_scroll < 0)
						help_scroll = 0;
					dirty = 1;
				}
			} else if (PAD_justRepeated(BTN_DOWN)) {
				help_scroll += scroll_step;
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				help_scroll = 0;
				state = help_return_state;
				dirty = 1;
			}
		}

		// Handle power management
		if (!screen_off && !ModuleCommon_isScreenOffHintActive()) {
			ModuleCommon_PWR_update(&dirty, &show_setting);
		}

		// Render
		if (dirty && !screen_off) {
			if (ModuleCommon_isScreenOffHintActive()) {
				GFX_clear(screen);
				render_screen_off_hint(screen);
			} else {
				switch (state) {
				case RADIO_INTERNAL_LIST:
					render_radio_list(screen, show_setting, radio_selected, &radio_scroll,
									  radio_toast_message, radio_toast_time);
					break;
				case RADIO_INTERNAL_PLAYING: {
					render_radio_playing(screen, show_setting, radio_selected);
					const RadioMetadata* meta = Radio_getMetadata();
					strncpy(last_rendered_artist, meta->artist, sizeof(last_rendered_artist) - 1);
					last_rendered_artist[sizeof(last_rendered_artist) - 1] = '\0';
					strncpy(last_rendered_title, meta->title, sizeof(last_rendered_title) - 1);
					last_rendered_title[sizeof(last_rendered_title) - 1] = '\0';
					break;
				}
				case RADIO_INTERNAL_ADD_COUNTRY:
					render_radio_add(screen, show_setting, add_country_selected, &add_country_scroll);
					break;
				case RADIO_INTERNAL_ADD_STATIONS:
					render_radio_add_stations(screen, show_setting, add_selected_country_code,
											  add_station_selected, &add_station_scroll,
											  sorted_station_indices, sorted_station_count,
											  radio_toast_message, radio_toast_time);
					break;
				case RADIO_INTERNAL_HELP:
					render_radio_help(screen, show_setting, &help_scroll);
					break;
				}
			}

			GFX_flip(screen);
			dirty = 0;

			// Keep refreshing while toast is visible
			if (state == RADIO_INTERNAL_LIST || state == RADIO_INTERNAL_ADD_STATIONS) {
				ModuleCommon_tickToast(radio_toast_message, radio_toast_time, &dirty);
			}
		} else if (!screen_off) {
			GFX_sync();
		}
	}
}
