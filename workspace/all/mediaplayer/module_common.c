#include <string.h>
#include <msettings.h>
#include "api.h"
#include "module_common.h"
#include "ui_components.h"
#include "ui_toast.h"
#include "ui_main.h"

static bool autosleep_disabled = false;

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
	show_quit_confirm = false;
	show_controls_help = false;
	start_was_pressed = false;
	overlay_buttons_were_active = false;
	overlay_release_time = 0;
}

GlobalInputResult ModuleCommon_handleGlobalInput(SDL_Surface* screen, IndicatorType* show_setting, int app_state) {
	GlobalInputResult result = {false, false, false};

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
		UI_renderConfirmDialog(screen, "Quit Media Player?", NULL);
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
			// Clear GPU scroll text layer so dialog is not obscured
			GFX_clearLayers(LAYER_SCROLLTEXT);
			PLAT_GPU_Flip();
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
		bool dirty_before = result.dirty ? true : false;
		bool dirty_tmp = dirty_before;
		PWR_update(&dirty_tmp, show_setting, NULL, NULL);

		if (dirty_tmp && !dirty_before) {
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
		PWR_enableAutosleep();
		autosleep_disabled = false;
	}
}

void ModuleCommon_quit(void) {
	// Ensure autosleep is re-enabled
	if (autosleep_disabled) {
		PWR_enableAutosleep();
		autosleep_disabled = false;
	}

	// Clear GPU scroll text layer
	GFX_clearLayers(LAYER_SCROLLTEXT);
}

void ModuleCommon_PWR_update(bool* dirty, IndicatorType* show_setting) {
	// Track overlay-triggering buttons for auto-hide (check BEFORE PWR_update)
	// MENU = brightness, SELECT = color temp, PLUS/MINUS = volume
	bool overlay_buttons_active = PAD_isPressed(BTN_PLUS) || PAD_isPressed(BTN_MINUS) || PAD_isPressed(BTN_MENU) || PAD_isPressed(BTN_SELECT);

	if (overlay_buttons_were_active && !overlay_buttons_active) {
		// Buttons just released - start timer
		overlay_release_time = SDL_GetTicks();
	}

	// Call platform PWR_update
	PWR_update(dirty, show_setting, NULL, NULL);

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
