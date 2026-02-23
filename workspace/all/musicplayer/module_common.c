#include <string.h>
#include <time.h>
#include <msettings.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "ui_toast.h"
#include "module_player.h"
#include "settings.h"
#include "ui_main.h"
#include "ui_components.h"
#include "ui_music.h"
#include "ui_radio.h"
#include "player.h"
#include "radio.h"
#include "spectrum.h"
#include "background.h"

static bool autosleep_disabled = false;
static uint32_t last_input_time = 0;

// Screen off hint state
static bool screen_off_hint_active = false;
static uint32_t screen_off_hint_start = 0;
static time_t screen_off_hint_start_wallclock = 0;

// Dialog states
static bool show_quit_confirm = false;
static bool show_controls_help = false;

// START button long press detection
static uint32_t start_press_time = 0;
static bool start_was_pressed = false;
#define START_LONG_PRESS_MS 500

// Overlay state tracking - force hide after button release
static bool overlay_buttons_were_active = false;
static uint32_t overlay_release_time = 0;
#define OVERLAY_VISIBLE_AFTER_RELEASE_MS 800 // How long overlay stays visible after release
#define OVERLAY_FORCE_HIDE_DURATION_MS 500	 // How long to keep forcing hide

void ModuleCommon_tickToast(char* message, uint32_t toast_time, bool* dirty) {
	if (message[0] == '\0')
		return;
	if (SDL_GetTicks() - toast_time < TOAST_DURATION) {
		*dirty = 1;
	} else {
		message[0] = '\0';
		*dirty = 1;
	}
}

void ModuleCommon_init(void) {
	autosleep_disabled = false;
	last_input_time = SDL_GetTicks();
	screen_off_hint_active = false;
	show_quit_confirm = false;
	show_controls_help = false;
	start_was_pressed = false;
	overlay_buttons_were_active = false;
	overlay_release_time = 0;
}

GlobalInputResult ModuleCommon_handleGlobalInput(SDL_Surface* screen, IndicatorType* show_setting, int app_state) {
	GlobalInputResult result = {false, false, false};

	// Poll USB HID events (earphone buttons)
	USBHIDEvent hid_event;
	while ((hid_event = Player_pollUSBHID()) != USB_HID_EVENT_NONE) {
		if (ModuleCommon_handleHIDVolume(hid_event)) {
			result.dirty = true;
			result.input_consumed = true;
		} else if (hid_event == USB_HID_EVENT_PLAY_PAUSE) {
			// Check what's currently playing and handle accordingly
			// Check radio first (takes priority when streaming)
			RadioState radio_state = Radio_getState();
			PlayerState player_state = Player_getState();

			if (radio_state == RADIO_STATE_PLAYING || radio_state == RADIO_STATE_BUFFERING) {
				// Radio is streaming - stop it
				Radio_stop();
				result.dirty = true;
				result.input_consumed = true;
			} else if (player_state == PLAYER_STATE_PLAYING || player_state == PLAYER_STATE_PAUSED) {
				// Music player is active - toggle pause
				Player_togglePause();
				result.dirty = true;
				result.input_consumed = true;
			} else {
				// Nothing playing - check if we can resume radio
				const char* last_url = Radio_getCurrentUrl();
				if (last_url && last_url[0] != '\0') {
					// Resume last radio station
					Radio_play(last_url);
					result.dirty = true;
					result.input_consumed = true;
				}
			}
		} else if (hid_event == USB_HID_EVENT_NEXT_TRACK || hid_event == USB_HID_EVENT_PREV_TRACK) {
			// Next/previous track - check what's currently active
			RadioState radio_state = Radio_getState();

			if (radio_state == RADIO_STATE_PLAYING || radio_state == RADIO_STATE_BUFFERING || radio_state == RADIO_STATE_CONNECTING) {
				// Radio is active - switch stations
				RadioStation* stations;
				int station_count = Radio_getStations(&stations);
				if (station_count > 1) {
					// Find current station index
					int current_idx = Radio_findCurrentStationIndex();
					if (current_idx < 0)
						continue;
					// Calculate new index
					int new_idx;
					if (hid_event == USB_HID_EVENT_NEXT_TRACK) {
						new_idx = (current_idx + 1) % station_count;
					} else {
						new_idx = (current_idx - 1 + station_count) % station_count;
					}
					// Switch to new station
					Radio_stop();
					Radio_play(stations[new_idx].url);
					result.dirty = true;
					result.input_consumed = true;
				}
			} else if (PlayerModule_isActive()) {
				// Music player is active - next/previous song
				if (hid_event == USB_HID_EVENT_NEXT_TRACK) {
					PlayerModule_nextTrack();
				} else {
					PlayerModule_prevTrack();
				}
				result.dirty = true;
				result.input_consumed = true;
			}
		}
	}

	// Handle volume controls - only when NOT in a combo with MENU or SELECT
	// (Menu + Vol = brightness, Select + Vol = color temp - handled by platform)
	// Note: We don't consume input or return early here - let PWR_update detect
	// the volume button press and set show_setting to display the volume UI
	ModuleCommon_handleHardwareVolume();

	// Handle quit confirmation dialog
	if (show_quit_confirm) {
		if (PAD_justPressed(BTN_A)) {
			show_quit_confirm = false;
			result.input_consumed = true;
			result.should_quit = true;
			return result;
		} else if (PAD_justPressed(BTN_B) || PAD_justPressed(BTN_START)) {
			show_quit_confirm = false;
			result.input_consumed = true;
			result.dirty = true;
			return result;
		}
		// Dialog is shown, consume input and render (covers entire screen)
		UI_renderConfirmDialog(screen, "Quit Music Player?", NULL);
		GFX_flip(screen);
		result.input_consumed = true;
		return result;
	}

	// Handle controls help dialog - press any button to close
	if (show_controls_help) {
		if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B) || PAD_justPressed(BTN_X) ||
			PAD_justPressed(BTN_Y) || PAD_justPressed(BTN_START) || PAD_justPressed(BTN_SELECT) ||
			PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_DOWN) ||
			PAD_justPressed(BTN_LEFT) || PAD_justPressed(BTN_RIGHT) ||
			PAD_justPressed(BTN_L1) || PAD_justPressed(BTN_R1) || PAD_justPressed(BTN_MENU)) {
			show_controls_help = false;
			result.input_consumed = true;
			result.dirty = true;
			return result;
		}
		// Dialog is shown, consume input and render (covers entire screen)
		render_controls_help(screen, app_state);
		GFX_flip(screen);
		result.input_consumed = true;
		return result;
	}

	// Handle START button - track press time for short/long press detection
	if (PAD_justPressed(BTN_START)) {
		start_press_time = SDL_GetTicks();
		start_was_pressed = true;
		result.input_consumed = true;
		return result;
	} else if (start_was_pressed) {
		bool show_dialog = false;

		if (PAD_isPressed(BTN_START)) {
			// Check for long press threshold while button is held
			uint32_t hold_time = SDL_GetTicks() - start_press_time;
			if (hold_time >= START_LONG_PRESS_MS) {
				show_quit_confirm = true;
				show_dialog = true;
			}
		} else if (PAD_justReleased(BTN_START)) {
			// Short press - show controls help
			show_controls_help = true;
			show_dialog = true;
		}

		if (show_dialog) {
			start_was_pressed = false;
			// Clear all GPU layers so dialog is not obscured
			GFX_clearLayers(LAYER_SCROLLTEXT);
			PLAT_clearLayers(LAYER_SPECTRUM);
			PLAT_clearLayers(LAYER_PLAYTIME);
			PLAT_GPU_Flip();
			PlayTime_clear();
			result.input_consumed = true;
			result.dirty = true;
			return result;
		}

		// Still waiting for press/release
		result.input_consumed = true;
		return result;
	}

	// Handle power management
	{
		bool dirty_tmp = result.dirty;
		IndicatorType indicator = (IndicatorType)*show_setting;
		PWR_update(&dirty_tmp, &indicator, NULL, NULL);
		*show_setting = (int)indicator;

		if (dirty_tmp && !result.dirty) {
			result.dirty = true;
		}
	}

	return result;
}

void ModuleCommon_setAutosleepDisabled(bool disabled) {
	if (disabled && !autosleep_disabled) {
		PWR_disableAutosleep();
		autosleep_disabled = true;
	} else if (!disabled && autosleep_disabled) {
		// Don't re-enable autosleep if background audio is still playing
		if (!Background_isPlaying()) {
			PWR_enableAutosleep();
			autosleep_disabled = false;
		}
	}
}

bool ModuleCommon_isScreenOffHintActive(void) {
	return screen_off_hint_active;
}

void ModuleCommon_startScreenOffHint(void) {
	screen_off_hint_active = true;
	screen_off_hint_start = SDL_GetTicks();
	screen_off_hint_start_wallclock = time(NULL);
}

void ModuleCommon_resetScreenOffHint(void) {
	screen_off_hint_active = false;
}

void ModuleCommon_recordInputTime(void) {
	last_input_time = SDL_GetTicks();
}

bool ModuleCommon_checkAutoScreenOffTimeout(void) {
	if (screen_off_hint_active)
		return false;
	uint32_t screen_timeout_ms = Settings_getScreenOffTimeout() * 1000;
	if (screen_timeout_ms > 0 && SDL_GetTicks() - last_input_time >= screen_timeout_ms) {
		ModuleCommon_startScreenOffHint();
		return true;
	}
	return false;
}

bool ModuleCommon_processScreenOffHintTimeout(void) {
	if (!screen_off_hint_active)
		return false;
	uint32_t now = SDL_GetTicks();
	time_t now_wallclock = time(NULL);
	bool timeout_sdl = (now - screen_off_hint_start >= SCREEN_OFF_HINT_DURATION_MS);
	bool timeout_wallclock = (now_wallclock - screen_off_hint_start_wallclock >= (SCREEN_OFF_HINT_DURATION_MS / 1000));
	if (timeout_sdl || timeout_wallclock) {
		screen_off_hint_active = false;
		PLAT_enableBacklight(0);
		return true;
	}
	return false;
}

void ModuleCommon_quit(void) {
	// Ensure autosleep is re-enabled
	if (autosleep_disabled) {
		PWR_enableAutosleep();
		autosleep_disabled = false;
	}

	// Clear all GPU layers
	GFX_clearLayers(LAYER_SCROLLTEXT);
	PLAT_clearLayers(LAYER_SPECTRUM);
	PLAT_clearLayers(LAYER_PLAYTIME);
	PLAT_clearLayers(LAYER_BUFFER);
}

void ModuleCommon_PWR_update(bool* dirty, IndicatorType* show_setting) {
	// Track overlay-triggering buttons for auto-hide (check BEFORE PWR_update)
	// MENU = brightness, SELECT = color temp, PLUS/MINUS = volume
	bool overlay_buttons_active = PAD_isPressed(BTN_PLUS) || PAD_isPressed(BTN_MINUS) || PAD_isPressed(BTN_MENU) || PAD_isPressed(BTN_SELECT);

	if (overlay_buttons_were_active && !overlay_buttons_active) {
		// Buttons just released - start timer
		overlay_release_time = SDL_GetTicks();
	}

	// Call platform PWR_update (convert int* to expected types)
	bool dirty_bool = *dirty != 0;
	IndicatorType indicator = (IndicatorType)*show_setting;
	PWR_update(&dirty_bool, &indicator, NULL, NULL);
	*dirty = dirty_bool ? 1 : 0;
	*show_setting = (int)indicator;

	// After visible period, force hide overlay
	if (overlay_release_time > 0) {
		uint32_t elapsed = SDL_GetTicks() - overlay_release_time;
		if (elapsed >= OVERLAY_VISIBLE_AFTER_RELEASE_MS) {
			// Visible period passed, now force hide
			*show_setting = INDICATOR_NONE;
			*dirty = 1;
			// Stop forcing after the duration
			if (elapsed >= OVERLAY_VISIBLE_AFTER_RELEASE_MS + OVERLAY_FORCE_HIDE_DURATION_MS) {
				overlay_release_time = 0;
			}
		}
	}

	overlay_buttons_were_active = overlay_buttons_active;
}

bool ModuleCommon_handleHIDVolume(USBHIDEvent hid_event) {
	if (hid_event != USB_HID_EVENT_VOLUME_UP && hid_event != USB_HID_EVENT_VOLUME_DOWN) {
		return false;
	}
	int vol = GetVolume();
	if (hid_event == USB_HID_EVENT_VOLUME_UP) {
		vol = (vol < 20) ? vol + 1 : 20;
	} else {
		vol = (vol > 0) ? vol - 1 : 0;
	}
	// USB HID events only come from USB DAC, so always use software volume
	SetVolume(vol);
	float v = vol / 20.0f;
	Player_setVolume(v * v * v);
	return true;
}

void ModuleCommon_handleHardwareVolume(void) {
	if (PAD_isPressed(BTN_MENU) || PAD_isPressed(BTN_SELECT))
		return;
	int vol_delta = 0;
	if (PAD_justRepeated(BTN_PLUS))
		vol_delta = 1;
	else if (PAD_justRepeated(BTN_MINUS))
		vol_delta = -1;
	if (!vol_delta)
		return;

	int vol = GetVolume() + vol_delta;
	if (vol > 20)
		vol = 20;
	else if (vol < 0)
		vol = 0;

	if (Player_isBluetoothActive() || Player_isUSBDACActive()) {
		float v = vol / 20.0f;
		Player_setVolume(v * v * v);
	} else {
		SetVolume(vol);
		Player_setVolume(1.0f);
	}
}
