/*
 * settings_led.c - LED Control integration for NextUI Settings
 *
 * Provides per-zone LED effect, color, speed, and brightness settings
 * within the settings framework, replacing the standalone ledcontrol app.
 */

#include "settings_led.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

#if MAX_LIGHTS == 0

/* No LED support on this platform */
SettingsPage* led_page_create(void) {
	return NULL;
}
void led_page_destroy(SettingsPage* page) {
	(void)page;
}

#else /* MAX_LIGHTS > 0 */

// ============================================
// Color arrays (from settings.c)
// ============================================

#define COLOR_COUNT 110
extern const int color_values[COLOR_COUNT];
extern const char* color_labels[COLOR_COUNT];

// ============================================
// Effect name arrays
// ============================================

#define STANDARD_EFFECT_COUNT 15
static const char* standard_effect_names[STANDARD_EFFECT_COUNT] = {
	"Linear", "Breathe", "Interval Breathe", "Static",
	"Blink 1", "Blink 2", "Blink 3", "Rainbow", "Twinkle",
	"Fire", "Glitter", "NeonGlow", "Firefly", "Aurora", "Reactive"};

#define TOPBAR_EFFECT_COUNT 17
static const char* topbar_effect_names[TOPBAR_EFFECT_COUNT] = {
	"Linear", "Breathe", "Interval Breathe", "Static",
	"Blink 1", "Blink 2", "Blink 3", "Rainbow", "Twinkle",
	"Fire", "Glitter", "NeonGlow", "Firefly", "Aurora", "Reactive",
	"Topbar Rainbow", "Topbar night"};

#define LR_EFFECT_COUNT 17
static const char* lr_effect_names[LR_EFFECT_COUNT] = {
	"Linear", "Breathe", "Interval Breathe", "Static",
	"Blink 1", "Blink 2", "Blink 3", "Rainbow", "Twinkle",
	"Fire", "Glitter", "NeonGlow", "Firefly", "Aurora", "Reactive",
	"LR Rainbow", "LR Reactive"};

/* Effect values are 1-based so settings_item_sync() maps correctly */
static int standard_effect_values[STANDARD_EFFECT_COUNT];
static int topbar_effect_values[TOPBAR_EFFECT_COUNT];
static int lr_effect_values[LR_EFFECT_COUNT];

// ============================================
// Speed labels: 50 entries "0"..."4900" (step 100)
// ============================================

#define SPEED_LABEL_COUNT 50
static char speed_label_buf[SPEED_LABEL_COUNT][8];
static const char* speed_labels[SPEED_LABEL_COUNT];
static int speed_values[SPEED_LABEL_COUNT];

// ============================================
// Brightness labels: 21 entries "0"..."100" (step 5)
// ============================================

#define LED_BRIGHTNESS_LABEL_COUNT 21
static char led_brightness_label_buf[LED_BRIGHTNESS_LABEL_COUNT][8];
static const char* led_brightness_labels[LED_BRIGHTNESS_LABEL_COUNT];
static int led_brightness_values[LED_BRIGHTNESS_LABEL_COUNT];

// ============================================
// Device info
// ============================================

static int led_is_brick = 0;
static int led_num_lights = 0;

// ============================================
// Generate dynamic labels
// ============================================

static int led_labels_initialized = 0;

static void led_init_labels(void) {
	if (led_labels_initialized)
		return;
	led_labels_initialized = 1;

	/* Effect values: 1-based */
	for (int i = 0; i < STANDARD_EFFECT_COUNT; i++)
		standard_effect_values[i] = i + 1;
	for (int i = 0; i < TOPBAR_EFFECT_COUNT; i++)
		topbar_effect_values[i] = i + 1;
	for (int i = 0; i < LR_EFFECT_COUNT; i++)
		lr_effect_values[i] = i + 1;

	/* Speed labels: 0, 100, 200, ..., 4900 */
	for (int i = 0; i < SPEED_LABEL_COUNT; i++) {
		speed_values[i] = i * 100;
		snprintf(speed_label_buf[i], sizeof(speed_label_buf[i]), "%d", i * 100);
		speed_labels[i] = speed_label_buf[i];
	}

	/* Brightness labels: 0, 5, 10, ..., 100 */
	for (int i = 0; i < LED_BRIGHTNESS_LABEL_COUNT; i++) {
		led_brightness_values[i] = i * 5;
		snprintf(led_brightness_label_buf[i], sizeof(led_brightness_label_buf[i]), "%d", i * 5);
		led_brightness_labels[i] = led_brightness_label_buf[i];
	}
}

// ============================================
// Save settings (identical format to ledcontrol)
// ============================================

static void led_save_settings(void) {
	char diskfilename[256];
	if (led_is_brick) {
		snprintf(diskfilename, sizeof(diskfilename), SHARED_USERDATA_PATH "/ledsettings_brick.txt");
	} else {
		snprintf(diskfilename, sizeof(diskfilename), SHARED_USERDATA_PATH "/ledsettings.txt");
	}

	FILE* file = fopen(diskfilename, "w");
	if (file == NULL) {
		LOG_error("Unable to open LED settings file for writing\n");
		return;
	}

	for (int i = 0; i < led_num_lights; i++) {
		fprintf(file, "[%s]\n", lightsDefault[i].name);
		fprintf(file, "effect=%d\n", lightsDefault[i].effect);
		fprintf(file, "color1=0x%06X\n", lightsDefault[i].color1);
		fprintf(file, "color2=0x%06X\n", lightsDefault[i].color2);
		fprintf(file, "speed=%d\n", lightsDefault[i].speed);
		fprintf(file, "brightness=%d\n", lightsDefault[i].brightness);
		fprintf(file, "trigger=%d\n", lightsDefault[i].trigger);
		fprintf(file, "filename=%s\n", lightsDefault[i].filename);
		fprintf(file, "inbrightness=%i\n", lightsDefault[i].inbrightness);
		fprintf(file, "\n");
	}

	fclose(file);
}

static void led_apply_and_save(void) {
	led_save_settings();
	LEDS_initLeds();
	// Push updated values to hardware so changes are visible immediately.
	// Don't use LEDS_setProfile here — that would clobber the active
	// operational profile (charging, low-battery, etc.).
	LEDS_updateLeds(false);
}

// ============================================
// Forward declarations for zone items (needed by sync helpers)
// ============================================

#define MAX_ZONE_ITEMS 5
static SettingItem zone_items[MAX_LIGHTS][MAX_ZONE_ITEMS];

#define BRIGHTNESS_ITEM_IDX 3
#define INBRIGHTNESS_ITEM_IDX 4

// ============================================
// Brightness sync helpers
// ============================================

/*
 * Hardware brightness paths are shared between zones:
 *   Brick: F1+F2 share max_scale_f1f2 (F2 writes are skipped by platform)
 *          Topbar has max_scale, L&R has max_scale_lr
 *   Non-brick: all zones share max_scale
 *
 * When one zone's brightness changes, sync the coupled zones'
 * stored values and UI items so everything stays consistent.
 */
static void led_sync_coupled_brightness(int source_zone) {
	int val = lightsDefault[source_zone].brightness;
	if (led_is_brick) {
		/* F1 (zone 0) and F2 (zone 1) share brightness */
		if (source_zone <= 1) {
			lightsDefault[0].brightness = val;
			lightsDefault[1].brightness = val;
			settings_item_sync(&zone_items[0][BRIGHTNESS_ITEM_IDX]);
			settings_item_sync(&zone_items[1][BRIGHTNESS_ITEM_IDX]);
		}
	} else {
		/* All zones share brightness */
		for (int z = 0; z < led_num_lights; z++) {
			lightsDefault[z].brightness = val;
			settings_item_sync(&zone_items[z][BRIGHTNESS_ITEM_IDX]);
		}
	}
}

static void led_sync_coupled_inbrightness(int source_zone) {
	int val = lightsDefault[source_zone].inbrightness;
	if (led_is_brick) {
		if (source_zone <= 1) {
			lightsDefault[0].inbrightness = val;
			lightsDefault[1].inbrightness = val;
			settings_item_sync(&zone_items[0][INBRIGHTNESS_ITEM_IDX]);
			settings_item_sync(&zone_items[1][INBRIGHTNESS_ITEM_IDX]);
		}
	} else {
		for (int z = 0; z < led_num_lights; z++) {
			lightsDefault[z].inbrightness = val;
			settings_item_sync(&zone_items[z][INBRIGHTNESS_ITEM_IDX]);
		}
	}
}

// ============================================
// Per-zone callbacks (macro-generated)
// ============================================

#define LED_ZONE_CALLBACKS(Z)                       \
	static int led_get_effect_##Z(void) {           \
		return lightsDefault[Z].effect;             \
	}                                               \
	static void led_set_effect_##Z(int val) {       \
		lightsDefault[Z].effect = val;              \
		led_apply_and_save();                       \
	}                                               \
	static int led_get_color_##Z(void) {            \
		return (int)lightsDefault[Z].color1;        \
	}                                               \
	static void led_set_color_##Z(int val) {        \
		lightsDefault[Z].color1 = (uint32_t)val;    \
		led_apply_and_save();                       \
	}                                               \
	static int led_get_speed_##Z(void) {            \
		return lightsDefault[Z].speed;              \
	}                                               \
	static void led_set_speed_##Z(int val) {        \
		lightsDefault[Z].speed = val;               \
		led_apply_and_save();                       \
	}                                               \
	static int led_get_brightness_##Z(void) {       \
		return lightsDefault[Z].brightness;         \
	}                                               \
	static void led_set_brightness_##Z(int val) {   \
		lightsDefault[Z].brightness = val;          \
		led_apply_and_save();                       \
		led_sync_coupled_brightness(Z);             \
	}                                               \
	static int led_get_inbrightness_##Z(void) {     \
		return lightsDefault[Z].inbrightness;       \
	}                                               \
	static void led_set_inbrightness_##Z(int val) { \
		lightsDefault[Z].inbrightness = val;        \
		led_apply_and_save();                       \
		led_sync_coupled_inbrightness(Z);           \
	}

LED_ZONE_CALLBACKS(0)
LED_ZONE_CALLBACKS(1)
LED_ZONE_CALLBACKS(2)
LED_ZONE_CALLBACKS(3)

/* Lookup tables for zone callbacks */
typedef int (*led_get_fn)(void);
typedef void (*led_set_fn)(int);

static led_get_fn zone_get_effect[] = {led_get_effect_0, led_get_effect_1, led_get_effect_2, led_get_effect_3};
static led_set_fn zone_set_effect[] = {led_set_effect_0, led_set_effect_1, led_set_effect_2, led_set_effect_3};
static led_get_fn zone_get_color[] = {led_get_color_0, led_get_color_1, led_get_color_2, led_get_color_3};
static led_set_fn zone_set_color[] = {led_set_color_0, led_set_color_1, led_set_color_2, led_set_color_3};
static led_get_fn zone_get_speed[] = {led_get_speed_0, led_get_speed_1, led_get_speed_2, led_get_speed_3};
static led_set_fn zone_set_speed[] = {led_set_speed_0, led_set_speed_1, led_set_speed_2, led_set_speed_3};
static led_get_fn zone_get_brightness[] = {led_get_brightness_0, led_get_brightness_1, led_get_brightness_2, led_get_brightness_3};
static led_set_fn zone_set_brightness[] = {led_set_brightness_0, led_set_brightness_1, led_set_brightness_2, led_set_brightness_3};
static led_get_fn zone_get_inbrightness[] = {led_get_inbrightness_0, led_get_inbrightness_1, led_get_inbrightness_2, led_get_inbrightness_3};
static led_set_fn zone_set_inbrightness[] = {led_set_inbrightness_0, led_set_inbrightness_1, led_set_inbrightness_2, led_set_inbrightness_3};

// ============================================
// Zone page construction
// ============================================

static SettingsPage zone_pages[MAX_LIGHTS];

static void led_build_zone_page(int zone_idx, const char* title,
								const char** eff_names, int* eff_values, int eff_count) {
	int idx = 0;

	zone_items[zone_idx][idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Effect", "LED light effect",
		eff_names, eff_count, eff_values,
		zone_get_effect[zone_idx], zone_set_effect[zone_idx], NULL);

	zone_items[zone_idx][idx++] = (SettingItem)ITEM_COLOR_INIT(
		"Color", "LED color",
		color_labels, COLOR_COUNT, (int*)color_values,
		zone_get_color[zone_idx], zone_set_color[zone_idx], NULL);

	zone_items[zone_idx][idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Speed", "Animation speed",
		speed_labels, SPEED_LABEL_COUNT, speed_values,
		zone_get_speed[zone_idx], zone_set_speed[zone_idx], NULL);

	const char* brightness_name = led_is_brick ? "Brightness" : "Brightness (All LEDs)";
	zone_items[zone_idx][idx++] = (SettingItem)ITEM_CYCLE_INIT(
		brightness_name, "LED brightness level",
		led_brightness_labels, LED_BRIGHTNESS_LABEL_COUNT, led_brightness_values,
		zone_get_brightness[zone_idx], zone_set_brightness[zone_idx], NULL);

	const char* inbrightness_name = led_is_brick ? "Info Brightness" : "Info Brightness (All LEDs)";
	zone_items[zone_idx][idx++] = (SettingItem)ITEM_CYCLE_INIT(
		inbrightness_name, "LED brightness during charging/low battery",
		led_brightness_labels, LED_BRIGHTNESS_LABEL_COUNT, led_brightness_values,
		zone_get_inbrightness[zone_idx], zone_set_inbrightness[zone_idx], NULL);

	memset(&zone_pages[zone_idx], 0, sizeof(SettingsPage));
	zone_pages[zone_idx].title = title;
	zone_pages[zone_idx].items = zone_items[zone_idx];
	zone_pages[zone_idx].item_count = idx;
	zone_pages[zone_idx].selected = 0;
	zone_pages[zone_idx].scroll = 0;
	zone_pages[zone_idx].is_list = 0;
	zone_pages[zone_idx].dynamic_start = -1;
	zone_pages[zone_idx].max_items = idx;
}

// ============================================
// Root LED page
// ============================================

#define MAX_LED_ROOT_ITEMS MAX_LIGHTS
static SettingItem led_root_items[MAX_LED_ROOT_ITEMS];
static SettingsPage led_root_page;

/* Zone title strings (static storage for page titles) */
static const char* brick_zone_titles[] = {"F1 key", "F2 key", "Top bar", "L&R triggers"};
static const char* default_zone_titles[] = {"Joystick L", "Joystick R", "Logo"};

SettingsPage* led_page_create(void) {
	char* device = getenv("DEVICE");
	if (exactMatch("brick", device)) {
		led_is_brick = 1;
		led_num_lights = 4;
	} else {
		led_is_brick = 0;
		led_num_lights = 3;
	}

	led_init_labels();

	const char** zone_titles = led_is_brick ? brick_zone_titles : default_zone_titles;

	/* Build per-zone pages */
	for (int z = 0; z < led_num_lights; z++) {
		const char** eff_names;
		int* eff_values;
		int eff_count;

		if (led_is_brick && z == 2) {
			/* Top bar */
			eff_names = topbar_effect_names;
			eff_values = topbar_effect_values;
			eff_count = TOPBAR_EFFECT_COUNT;
		} else if (led_is_brick && z == 3) {
			/* L&R triggers */
			eff_names = lr_effect_names;
			eff_values = lr_effect_values;
			eff_count = LR_EFFECT_COUNT;
		} else {
			/* Standard zones */
			eff_names = standard_effect_names;
			eff_values = standard_effect_values;
			eff_count = STANDARD_EFFECT_COUNT;
		}

		led_build_zone_page(z, zone_titles[z], eff_names, eff_values, eff_count);
	}

	/* Sync all zone items */
	for (int z = 0; z < led_num_lights; z++) {
		for (int i = 0; i < zone_pages[z].item_count; i++) {
			settings_item_sync(&zone_pages[z].items[i]);
		}
	}

	/* Build root list page */
	int idx = 0;
	for (int z = 0; z < led_num_lights; z++) {
		led_root_items[idx++] = (SettingItem)ITEM_SUBMENU_INIT(
			zone_titles[z], "", &zone_pages[z]);
	}

	memset(&led_root_page, 0, sizeof(SettingsPage));
	led_root_page.title = "LED Control";
	led_root_page.items = led_root_items;
	led_root_page.item_count = idx;
	led_root_page.selected = 0;
	led_root_page.scroll = 0;
	led_root_page.is_list = 1;
	led_root_page.dynamic_start = -1;
	led_root_page.max_items = idx;

	return &led_root_page;
}

void led_page_destroy(SettingsPage* page) {
	(void)page;
	/* Static allocation, nothing to free */
}

#endif /* MAX_LIGHTS == 0 */
