/*
 * settings.c - NextUI Settings application (C99 rewrite)
 *
 * Converted from settings.cpp to pure C.
 * Builds the full settings menu tree and runs the main loop.
 */

#include "msettings.h"
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "ra_auth.h"
#include "settings_menu.h"
#include "settings_wifi.h"
#include "settings_bt.h"
#include "ui_components.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================
// BusyBox stock version (for OS modification detection)
// ============================================

#define BUSYBOX_STOCK_VERSION "1.27.2"

// ============================================
// Signal handler
// ============================================

static volatile int app_quit = 0;

static void sig_handler(int sig) {
	if (sig == SIGINT || sig == SIGTERM)
		app_quit = 1;
}

// ============================================
// Device detection
// ============================================

typedef enum {
	PLAT_UNKNOWN = 0,
	PLAT_TG5040,
	PLAT_TG5050,
	PLAT_MY355
} DevicePlatform;

typedef enum {
	MODEL_UNKNOWN = 0,
	MODEL_BRICK,
	MODEL_SMARTPRO,
	MODEL_SMARTPROS,
	MODEL_FLIP
} DeviceModel;

typedef struct {
	DevicePlatform platform;
	DeviceModel model;
} DeviceInfo;

static DeviceInfo device_detect(void) {
	DeviceInfo dev = {PLAT_UNKNOWN, MODEL_UNKNOWN};
	const char* device = getenv("DEVICE");
	if (!device)
		return dev;

	if (exactMatch("brick", device)) {
		dev.model = MODEL_BRICK;
		dev.platform = PLAT_TG5040;
	} else if (exactMatch("smartpro", device)) {
		dev.model = MODEL_SMARTPRO;
		dev.platform = PLAT_TG5040;
	} else if (exactMatch("smartpros", device)) {
		dev.model = MODEL_SMARTPROS;
		dev.platform = PLAT_TG5050;
	} else if (exactMatch("my355", device)) {
		dev.model = MODEL_FLIP;
		dev.platform = PLAT_MY355;
	}
	return dev;
}

static int has_color_temp(const DeviceInfo* dev) {
	return dev->platform == PLAT_TG5040;
}

static int has_contrast_sat(const DeviceInfo* dev) {
	return dev->platform == PLAT_MY355 || dev->platform == PLAT_TG5040;
}

static int has_exposure(const DeviceInfo* dev) {
	return dev->platform == PLAT_TG5040;
}

static int has_active_cooling(const DeviceInfo* dev) {
	return dev->platform == PLAT_TG5050;
}

static int has_mute_toggle(const DeviceInfo* dev) {
	return dev->platform == PLAT_TG5050 || dev->platform == PLAT_TG5040;
}

static int has_analog_sticks(const DeviceInfo* dev) {
	return dev->model == MODEL_SMARTPRO || dev->model == MODEL_SMARTPROS;
}

static int has_wifi(const DeviceInfo* dev) {
	return dev->platform == PLAT_TG5050 || dev->platform == PLAT_TG5040 || dev->platform == PLAT_MY355;
}

static int has_bluetooth(const DeviceInfo* dev) {
	return dev->platform == PLAT_TG5050 || dev->platform == PLAT_TG5040 || dev->platform == PLAT_MY355;
}

// ============================================
// Command execution helper
// ============================================

static int exec_command(const char* cmd, char* output, int max_len) {
	char full_cmd[512];
	snprintf(full_cmd, sizeof(full_cmd), "%s 2>&1", cmd);
	FILE* fp = popen(full_cmd, "r");
	if (!fp)
		return -1;
	output[0] = '\0';
	char buf[128];
	int pos = 0;
	while (fgets(buf, sizeof(buf), fp) && pos < max_len - 1) {
		int len = (int)strlen(buf);
		if (pos + len >= max_len)
			len = max_len - pos - 1;
		memcpy(output + pos, buf, len);
		pos += len;
	}
	output[pos] = '\0';
	pclose(fp);
	return 0;
}

static void extract_busybox_version(const char* output, char* version_out, int max_len) {
	/* Search for "BusyBox vX.Y.Z" pattern */
	const char* p = strstr(output, "BusyBox v");
	if (!p) {
		version_out[0] = '\0';
		return;
	}
	p += 9; /* skip "BusyBox v" to point at "X.Y.Z" */
	int i = 0;
	while (p[i] && p[i] != ' ' && p[i] != '\n' && p[i] != '\r' && i < max_len - 1) {
		version_out[i] = p[i];
		i++;
	}
	version_out[i] = '\0';
}

// ============================================
// Color values and labels (110 entries)
// ============================================

#define COLOR_COUNT 110

static const int color_values[COLOR_COUNT] = {
	0x000022, 0x000044, 0x000066, 0x000088, 0x0000AA, 0x0000CC, 0x1E2329, 0x3366FF, 0x4D7AFF, 0x6699FF, 0x80B3FF, 0x99CCFF, 0xB3D9FF,
	0x002222, 0x004444, 0x006666, 0x008888, 0x00AAAA, 0x00CCCC, 0x33FFFF, 0x4DFFFF, 0x66FFFF, 0x80FFFF, 0x99FFFF, 0xB3FFFF,
	0x002200, 0x004400, 0x006600, 0x008800, 0x00AA00, 0x00CC00, 0x33FF33, 0x4DFF4D, 0x66FF66, 0x80FF80, 0x99FF99, 0xB3FFB3,
	0x220022, 0x440044, 0x660066, 0x880088, 0x9B2257, 0xAA00AA, 0xCC00CC, 0xFF33FF, 0xFF4DFF, 0xFF66FF, 0xFF80FF, 0xFF99FF, 0xFFB3FF,
	0x110022, 0x220044, 0x330066, 0x440088, 0x5500AA, 0x6600CC, 0x8833FF, 0x994DFF, 0xAA66FF, 0xBB80FF, 0xCC99FF, 0xDDB3FF,
	0x220000, 0x440000, 0x660000, 0x880000, 0xAA0000, 0xCC0000, 0xFF3333, 0xFF4D4D, 0xFF6666, 0xFF8080, 0xFF9999, 0xFFB3B3,
	0x222200, 0x444400, 0x666600, 0x888800, 0xAAAA00, 0xCCCC00, 0xFFFF33, 0xFFFF4D, 0xFFFF66, 0xFFFF80, 0xFFFF99, 0xFFFFB3,
	0x221100, 0x442200, 0x663300, 0x884400, 0xAA5500, 0xCC6600, 0xFF8833, 0xFF994D, 0xFFAA66, 0xFFBB80, 0xFFCC99, 0xFFDDB3,
	0x000000, 0x141414, 0x282828, 0x3C3C3C, 0x505050, 0x646464, 0x8C8C8C, 0xA0A0A0, 0xB4B4B4, 0xC8C8C8, 0xDCDCDC, 0xFFFFFF};

static const char* color_labels[COLOR_COUNT] = {
	"0x000022", "0x000044", "0x000066", "0x000088", "0x0000AA", "0x0000CC", "0x1E2329", "0x3366FF", "0x4D7AFF", "0x6699FF", "0x80B3FF", "0x99CCFF", "0xB3D9FF",
	"0x002222", "0x004444", "0x006666", "0x008888", "0x00AAAA", "0x00CCCC", "0x33FFFF", "0x4DFFFF", "0x66FFFF", "0x80FFFF", "0x99FFFF", "0xB3FFFF",
	"0x002200", "0x004400", "0x006600", "0x008800", "0x00AA00", "0x00CC00", "0x33FF33", "0x4DFF4D", "0x66FF66", "0x80FF80", "0x99FF99", "0xB3FFB3",
	"0x220022", "0x440044", "0x660066", "0x880088", "0x9B2257", "0xAA00AA", "0xCC00CC", "0xFF33FF", "0xFF4DFF", "0xFF66FF", "0xFF80FF", "0xFF99FF", "0xFFB3FF",
	"0x110022", "0x220044", "0x330066", "0x440088", "0x5500AA", "0x6600CC", "0x8833FF", "0x994DFF", "0xAA66FF", "0xBB80FF", "0xCC99FF", "0xDDB3FF",
	"0x220000", "0x440000", "0x660000", "0x880000", "0xAA0000", "0xCC0000", "0xFF3333", "0xFF4D4D", "0xFF6666", "0xFF8080", "0xFF9999", "0xFFB3B3",
	"0x222200", "0x444400", "0x666600", "0x888800", "0xAAAA00", "0xCCCC00", "0xFFFF33", "0xFFFF4D", "0xFFFF66", "0xFFFF80", "0xFFFF99", "0xFFFFB3",
	"0x221100", "0x442200", "0x663300", "0x884400", "0xAA5500", "0xCC6600", "0xFF8833", "0xFF994D", "0xFFAA66", "0xFFBB80", "0xFFCC99", "0xFFDDB3",
	"0x000000", "0x141414", "0x282828", "0x3C3C3C", "0x505050", "0x646464", "0x8C8C8C", "0xA0A0A0", "0xB4B4B4", "0xC8C8C8", "0xDCDCDC", "0xFFFFFF"};

// ============================================
// Static label/value arrays
// ============================================

static const char* font_labels[] = {"OG", "Next"};
static const char* on_off_labels[] = {"Off", "On"};

static int screen_timeout_values[] = {0, 5, 10, 15, 30, 45, 60, 90, 120, 240, 360, 600};
static const char* screen_timeout_labels[] = {"Never", "5s", "10s", "15s", "30s", "45s", "60s", "90s", "2m", "4m", "6m", "10m"};
#define SCREEN_TIMEOUT_COUNT 12

static int sleep_timeout_values[] = {5, 10, 15, 30, 45, 60, 90, 120, 240, 360, 600};
static const char* sleep_timeout_labels[] = {"5s", "10s", "15s", "30s", "45s", "60s", "90s", "2m", "4m", "6m", "10m"};
#define SLEEP_TIMEOUT_COUNT 11

/* Volume: 21 labels (Muted, 5%, 10%, ... 100%) */
static int volume_values[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
static const char* volume_labels[] = {
	"Muted", "5%", "10%", "15%", "20%", "25%", "30%", "35%", "40%", "45%", "50%",
	"55%", "60%", "65%", "70%", "75%", "80%", "85%", "90%", "95%", "100%"};
#define VOLUME_LABEL_COUNT 21

/* Notification duration */
static int notify_duration_values[] = {1, 2, 3, 4, 5};
static const char* notify_duration_labels[] = {"1s", "2s", "3s", "4s", "5s"};
#define NOTIFY_DURATION_COUNT 5

/* Progress notification duration */
static int progress_duration_values[] = {0, 1, 2, 3, 4, 5};
static const char* progress_duration_labels[] = {"Off", "1s", "2s", "3s", "4s", "5s"};
#define PROGRESS_DURATION_COUNT 6

/* RA sort order */
static int ra_sort_values[] = {
	RA_SORT_UNLOCKED_FIRST,
	RA_SORT_DISPLAY_ORDER_FIRST,
	RA_SORT_DISPLAY_ORDER_LAST,
	RA_SORT_WON_BY_MOST,
	RA_SORT_WON_BY_LEAST,
	RA_SORT_POINTS_MOST,
	RA_SORT_POINTS_LEAST,
	RA_SORT_TITLE_AZ,
	RA_SORT_TITLE_ZA,
	RA_SORT_TYPE_ASC,
	RA_SORT_TYPE_DESC};
static const char* ra_sort_labels[] = {
	"Unlocked First",
	"Display Order (First)",
	"Display Order (Last)",
	"Won By (Most)",
	"Won By (Least)",
	"Points (Most)",
	"Points (Least)",
	"Title (A-Z)",
	"Title (Z-A)",
	"Type (Asc)",
	"Type (Desc)"};
#define RA_SORT_LABEL_COUNT 11

/* Default view */
static int default_view_values[] = {SCREEN_GAMELIST, SCREEN_GAMESWITCHER, SCREEN_QUICKMENU};
static const char* default_view_labels[] = {"Content List", "Game Switcher", "Quick Menu"};
#define DEFAULT_VIEW_COUNT 3

/* Save format */
static int save_format_values[] = {SAVE_FORMAT_SAV, SAVE_FORMAT_SRM, SAVE_FORMAT_SRM_UNCOMPRESSED, SAVE_FORMAT_GEN};
static const char* save_format_labels[] = {"MinUI (default)", "Retroarch (compressed)", "Retroarch (uncompressed)", "Generic"};
#define SAVE_FORMAT_COUNT 4

/* State format */
static int state_format_values[] = {STATE_FORMAT_SAV, STATE_FORMAT_SRM_EXTRADOT, STATE_FORMAT_SRM_UNCOMRESSED_EXTRADOT, STATE_FORMAT_SRM, STATE_FORMAT_SRM_UNCOMRESSED};
static const char* state_format_labels[] = {"MinUI (default)", "Retroarch-ish (compressed)", "Retroarch-ish (uncompressed)", "Retroarch (compressed)", "Retroarch (uncompressed)"};
#define STATE_FORMAT_COUNT 5

/* Fan speed */
static int fan_speed_values[] = {-3, -2, -1, 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
static const char* fan_speed_labels[] = {"Performance", "Normal", "Quiet", "0%", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"};
#define FAN_SPEED_COUNT 14

/* Brightness (0-10): direct index mapping */
static const char* brightness_labels[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
#define BRIGHTNESS_LABEL_COUNT 11

/* Color temperature (0-40): generated at init */
static char colortemp_label_buf[41][4];
static const char* colortemp_labels[41];
#define COLORTEMP_LABEL_COUNT 41

/* Contrast (-4 to 5): 10 labels */
static const char* contrast_labels[] = {"-4", "-3", "-2", "-1", "0", "1", "2", "3", "4", "5"};
static int contrast_values[] = {-4, -3, -2, -1, 0, 1, 2, 3, 4, 5};
#define CONTRAST_LABEL_COUNT 10

/* Saturation (-5 to 5): 11 labels */
static const char* saturation_labels[] = {"-5", "-4", "-3", "-2", "-1", "0", "1", "2", "3", "4", "5"};
static int saturation_values[] = {-5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5};
#define SATURATION_LABEL_COUNT 11

/* Exposure (-4 to 5): 10 labels */
static const char* exposure_labels[] = {"-4", "-3", "-2", "-1", "0", "1", "2", "3", "4", "5"};
static int exposure_values[] = {-4, -3, -2, -1, 0, 1, 2, 3, 4, 5};
#define EXPOSURE_LABEL_COUNT 10

/* Thumbnail radius (0-24): direct index mapping */
static char thumb_radius_label_buf[25][4];
static const char* thumb_radius_labels[25];
#define THUMB_RADIUS_LABEL_COUNT 25

/* Game art width (5-100%): 96 labels, values start at 5 */
#define GAME_ART_WIDTH_COUNT 96
static char game_art_width_label_buf[GAME_ART_WIDTH_COUNT][5];
static const char* game_art_width_labels[GAME_ART_WIDTH_COUNT];
static int game_art_width_values[GAME_ART_WIDTH_COUNT];

/* On/off as int values 0,1 */
static int on_off_values[] = {0, 1};

/* Dpad mode: Dpad, Joystick, Both */
static const char* dpad_mode_labels[] = {"Dpad", "Joystick", "Both"};
static int dpad_mode_values[] = {0, 1, 2};
#define DPAD_MODE_COUNT 3

// ============================================
// FN Switch "when toggled" arrays
// ============================================

/* Volume when toggled: Unchanged, Muted, 5%, 10%, ... 100% (22 entries) */
#define MUTE_VOLUME_COUNT 22
static int mute_volume_values[MUTE_VOLUME_COUNT];
static const char* mute_volume_labels[MUTE_VOLUME_COUNT];

/* Brightness when toggled: Unchanged, 0-10 (12 entries) */
#define MUTE_BRIGHTNESS_COUNT 12
static int mute_brightness_values[MUTE_BRIGHTNESS_COUNT];
static char mute_brightness_label_buf[MUTE_BRIGHTNESS_COUNT][12];
static const char* mute_brightness_labels[MUTE_BRIGHTNESS_COUNT];

/* Color temp when toggled: Unchanged, 0-40 (42 entries) */
#define MUTE_COLORTEMP_COUNT 42
static int mute_colortemp_values[MUTE_COLORTEMP_COUNT];
static char mute_colortemp_label_buf[MUTE_COLORTEMP_COUNT][12];
static const char* mute_colortemp_labels[MUTE_COLORTEMP_COUNT];

/* Contrast when toggled: Unchanged, -4 to 5 (11 entries) */
#define MUTE_CONTRAST_COUNT 11
static int mute_contrast_values[MUTE_CONTRAST_COUNT];
static char mute_contrast_label_buf[MUTE_CONTRAST_COUNT][12];
static const char* mute_contrast_labels[MUTE_CONTRAST_COUNT];

/* Saturation when toggled: Unchanged, -5 to 5 (12 entries) */
#define MUTE_SATURATION_COUNT 12
static int mute_saturation_values[MUTE_SATURATION_COUNT];
static char mute_saturation_label_buf[MUTE_SATURATION_COUNT][12];
static const char* mute_saturation_labels[MUTE_SATURATION_COUNT];

/* Exposure when toggled: Unchanged, -4 to 5 (11 entries) */
#define MUTE_EXPOSURE_COUNT 11
static int mute_exposure_values[MUTE_EXPOSURE_COUNT];
static char mute_exposure_label_buf[MUTE_EXPOSURE_COUNT][12];
static const char* mute_exposure_labels[MUTE_EXPOSURE_COUNT];

// ============================================
// Timezone arrays (dynamic)
// ============================================

static char timezones[MAX_TIMEZONES][MAX_TZ_LENGTH];
static int tz_count = 0;
static const char** tz_labels = NULL;
static int* tz_values = NULL;

// ============================================
// Generate dynamic labels at init
// ============================================

static void init_dynamic_labels(void) {
	int i;

	/* Color temperature labels 0-40 */
	for (i = 0; i < COLORTEMP_LABEL_COUNT; i++) {
		snprintf(colortemp_label_buf[i], sizeof(colortemp_label_buf[i]), "%d", i);
		colortemp_labels[i] = colortemp_label_buf[i];
	}

	/* Thumbnail radius labels 0-24 */
	for (i = 0; i < THUMB_RADIUS_LABEL_COUNT; i++) {
		snprintf(thumb_radius_label_buf[i], sizeof(thumb_radius_label_buf[i]), "%d", i);
		thumb_radius_labels[i] = thumb_radius_label_buf[i];
	}

	/* Game art width labels 5-100 */
	for (i = 0; i < GAME_ART_WIDTH_COUNT; i++) {
		game_art_width_values[i] = i + 5;
		snprintf(game_art_width_label_buf[i], sizeof(game_art_width_label_buf[i]), "%d%%", i + 5);
		game_art_width_labels[i] = game_art_width_label_buf[i];
	}

	/* Mute volume: Unchanged, Muted, 5%, 10%, ... 100% */
	mute_volume_values[0] = (int)SETTINGS_DEFAULT_MUTE_NO_CHANGE;
	mute_volume_labels[0] = "Unchanged";
	for (i = 0; i < 21; i++) {
		mute_volume_values[i + 1] = i;
		mute_volume_labels[i + 1] = volume_labels[i]; /* reuse: Muted, 5%, 10%, ... 100% */
	}

	/* Mute brightness: Unchanged, 0-10 */
	mute_brightness_values[0] = (int)SETTINGS_DEFAULT_MUTE_NO_CHANGE;
	mute_brightness_labels[0] = "Unchanged";
	for (i = 0; i <= 10; i++) {
		mute_brightness_values[i + 1] = i;
		snprintf(mute_brightness_label_buf[i + 1], sizeof(mute_brightness_label_buf[i + 1]), "%d", i);
		mute_brightness_labels[i + 1] = mute_brightness_label_buf[i + 1];
	}

	/* Mute colortemp: Unchanged, 0-40 */
	mute_colortemp_values[0] = (int)SETTINGS_DEFAULT_MUTE_NO_CHANGE;
	mute_colortemp_labels[0] = "Unchanged";
	for (i = 0; i <= 40; i++) {
		mute_colortemp_values[i + 1] = i;
		snprintf(mute_colortemp_label_buf[i + 1], sizeof(mute_colortemp_label_buf[i + 1]), "%d", i);
		mute_colortemp_labels[i + 1] = mute_colortemp_label_buf[i + 1];
	}

	/* Mute contrast: Unchanged, -4 to 5 */
	mute_contrast_values[0] = (int)SETTINGS_DEFAULT_MUTE_NO_CHANGE;
	mute_contrast_labels[0] = "Unchanged";
	for (i = -4; i <= 5; i++) {
		int idx = i + 5; /* -4->1, -3->2, ..., 5->10 */
		mute_contrast_values[idx] = i;
		snprintf(mute_contrast_label_buf[idx], sizeof(mute_contrast_label_buf[idx]), "%d", i);
		mute_contrast_labels[idx] = mute_contrast_label_buf[idx];
	}

	/* Mute saturation: Unchanged, -5 to 5 */
	mute_saturation_values[0] = (int)SETTINGS_DEFAULT_MUTE_NO_CHANGE;
	mute_saturation_labels[0] = "Unchanged";
	for (i = -5; i <= 5; i++) {
		int idx = i + 6; /* -5->1, -4->2, ..., 5->11 */
		mute_saturation_values[idx] = i;
		snprintf(mute_saturation_label_buf[idx], sizeof(mute_saturation_label_buf[idx]), "%d", i);
		mute_saturation_labels[idx] = mute_saturation_label_buf[idx];
	}

	/* Mute exposure: Unchanged, -4 to 5 */
	mute_exposure_values[0] = (int)SETTINGS_DEFAULT_MUTE_NO_CHANGE;
	mute_exposure_labels[0] = "Unchanged";
	for (i = -4; i <= 5; i++) {
		int idx = i + 5; /* -4->1, -3->2, ..., 5->10 */
		mute_exposure_values[idx] = i;
		snprintf(mute_exposure_label_buf[idx], sizeof(mute_exposure_label_buf[idx]), "%d", i);
		mute_exposure_labels[idx] = mute_exposure_label_buf[idx];
	}

	/* Timezone labels */
	TIME_getTimezones(timezones, &tz_count);
	if (tz_count > 0) {
		tz_labels = (const char**)calloc(tz_count, sizeof(const char*));
		tz_values = (int*)calloc(tz_count, sizeof(int));
		for (i = 0; i < tz_count; i++) {
			tz_labels[i] = timezones[i];
			tz_values[i] = i;
		}
	}
}

static void free_dynamic_labels(void) {
	if (tz_labels) {
		free(tz_labels);
		tz_labels = NULL;
	}
	if (tz_values) {
		free(tz_values);
		tz_values = NULL;
	}
}

// ============================================
// Color index lookup helper
// ============================================

// ============================================
// Appearance callbacks
// ============================================

static int get_font(void) {
	return CFG_getFontId();
}
static void set_font(int v) {
	CFG_setFontId(v);
}
static void reset_font(void) {
	CFG_setFontId(CFG_DEFAULT_FONT_ID);
}

/* Color 1 - Main Color */
static int get_color1(void) {
	return (int)CFG_getColor(1);
}
static void set_color1(int val) {
	CFG_setColor(1, (uint32_t)val);
}
static void reset_color1(void) {
	CFG_setColor(1, CFG_DEFAULT_COLOR1);
}

/* Color 2 - Primary Accent */
static int get_color2(void) {
	return (int)CFG_getColor(2);
}
static void set_color2(int val) {
	CFG_setColor(2, (uint32_t)val);
}
static void reset_color2(void) {
	CFG_setColor(2, CFG_DEFAULT_COLOR2);
}

/* Color 3 - Secondary Accent */
static int get_color3(void) {
	return (int)CFG_getColor(3);
}
static void set_color3(int val) {
	CFG_setColor(3, (uint32_t)val);
}
static void reset_color3(void) {
	CFG_setColor(3, CFG_DEFAULT_COLOR3);
}

/* Color 6 - Hint Info */
static int get_color6(void) {
	return (int)CFG_getColor(6);
}
static void set_color6(int val) {
	CFG_setColor(6, (uint32_t)val);
}
static void reset_color6(void) {
	CFG_setColor(6, CFG_DEFAULT_COLOR6);
}

/* Color 4 - List Text */
static int get_color4(void) {
	return (int)CFG_getColor(4);
}
static void set_color4(int val) {
	CFG_setColor(4, (uint32_t)val);
}
static void reset_color4(void) {
	CFG_setColor(4, CFG_DEFAULT_COLOR4);
}

/* Color 5 - List Text Selected */
static int get_color5(void) {
	return (int)CFG_getColor(5);
}
static void set_color5(int val) {
	CFG_setColor(5, (uint32_t)val);
}
static void reset_color5(void) {
	CFG_setColor(5, CFG_DEFAULT_COLOR5);
}

/* Show battery percent */
static int get_show_battery_percent(void) {
	return CFG_getShowBatteryPercent() ? 1 : 0;
}
static void set_show_battery_percent(int v) {
	CFG_setShowBatteryPercent(v != 0);
}
static void reset_show_battery_percent(void) {
	CFG_setShowBatteryPercent(CFG_DEFAULT_SHOWBATTERYPERCENT);
}

/* Show menu animations */
static int get_menu_animations(void) {
	return CFG_getMenuAnimations() ? 1 : 0;
}
static void set_menu_animations(int v) {
	CFG_setMenuAnimations(v != 0);
}
static void reset_menu_animations(void) {
	CFG_setMenuAnimations(CFG_DEFAULT_SHOWMENUANIMATIONS);
}

/* Show menu transitions */
static int get_menu_transitions(void) {
	return CFG_getMenuTransitions() ? 1 : 0;
}
static void set_menu_transitions(int v) {
	CFG_setMenuTransitions(v != 0);
}
static void reset_menu_transitions(void) {
	CFG_setMenuTransitions(CFG_DEFAULT_SHOWMENUTRANSITIONS);
}

/* Game art corner radius */
static int get_thumb_radius(void) {
	return CFG_getThumbnailRadius();
}
static void set_thumb_radius(int v) {
	CFG_setThumbnailRadius(v);
}
static void reset_thumb_radius(void) {
	CFG_setThumbnailRadius(CFG_DEFAULT_THUMBRADIUS);
}

/* Game art width */
static int get_game_art_width(void) {
	return (int)(CFG_getGameArtWidth() * 100);
}
static void set_game_art_width(int val) {
	CFG_setGameArtWidth((double)val / 100.0);
}
static void reset_game_art_width(void) {
	CFG_setGameArtWidth(CFG_DEFAULT_GAMEARTWIDTH);
}

/* Show folder names at root */
static int get_show_folder_names(void) {
	return CFG_getShowFolderNamesAtRoot() ? 1 : 0;
}
static void set_show_folder_names(int v) {
	CFG_setShowFolderNamesAtRoot(v != 0);
}
static void reset_show_folder_names(void) {
	CFG_setShowFolderNamesAtRoot(CFG_DEFAULT_SHOWFOLDERNAMESATROOT);
}

/* Show Recents */
static int get_show_recents(void) {
	return CFG_getShowRecents() ? 1 : 0;
}
static void set_show_recents(int v) {
	CFG_setShowRecents(v != 0);
}
static void reset_show_recents(void) {
	CFG_setShowRecents(CFG_DEFAULT_SHOWRECENTS);
}

/* Show Tools */
static int get_show_tools(void) {
	return CFG_getShowTools() ? 1 : 0;
}
static void set_show_tools(int v) {
	CFG_setShowTools(v != 0);
}
static void reset_show_tools(void) {
	CFG_setShowTools(CFG_DEFAULT_SHOWTOOLS);
}

/* Show game art */
static int get_show_game_art(void) {
	return CFG_getShowGameArt() ? 1 : 0;
}
static void set_show_game_art(int v) {
	CFG_setShowGameArt(v != 0);
}
static void reset_show_game_art(void) {
	CFG_setShowGameArt(CFG_DEFAULT_SHOWGAMEART);
}

/* Use folder background for ROMs */
static int get_roms_use_folder_bg(void) {
	return CFG_getRomsUseFolderBackground() ? 1 : 0;
}
static void set_roms_use_folder_bg(int v) {
	CFG_setRomsUseFolderBackground(v != 0);
}
static void reset_roms_use_folder_bg(void) {
	CFG_setRomsUseFolderBackground(CFG_DEFAULT_ROMSUSEFOLDERBACKGROUND);
}

/* Show Quickswitcher UI */
static int get_show_quickswitcher(void) {
	return CFG_getShowQuickswitcherUI() ? 1 : 0;
}
static void set_show_quickswitcher(int v) {
	CFG_setShowQuickswitcherUI(v != 0);
}
static void reset_show_quickswitcher(void) {
	CFG_setShowQuickswitcherUI(CFG_DEFAULT_SHOWQUICKWITCHERUI);
}

// ============================================
// Display callbacks
// ============================================

static int get_brightness(void) {
	return GetBrightness();
}
static void set_brightness(int v) {
	SetBrightness(v);
}
static void reset_brightness(void) {
	SetBrightness(SETTINGS_DEFAULT_BRIGHTNESS);
}

static int get_colortemp(void) {
	return GetColortemp();
}
static void set_colortemp(int v) {
	SetColortemp(v);
}
static void reset_colortemp(void) {
	SetColortemp(SETTINGS_DEFAULT_COLORTEMP);
}

static int get_contrast(void) {
	return GetContrast();
}
static void set_contrast(int val) {
	SetContrast(val);
}
static void reset_contrast(void) {
	SetContrast(SETTINGS_DEFAULT_CONTRAST);
}

static int get_saturation(void) {
	return GetSaturation();
}
static void set_saturation(int val) {
	SetSaturation(val);
}
static void reset_saturation(void) {
	SetSaturation(SETTINGS_DEFAULT_SATURATION);
}

static int get_exposure(void) {
	return GetExposure();
}
static void set_exposure(int val) {
	SetExposure(val);
}
static void reset_exposure(void) {
	SetExposure(SETTINGS_DEFAULT_EXPOSURE);
}

// ============================================
// System callbacks
// ============================================

static int get_volume(void) {
	return GetVolume();
}
static void set_volume(int val) {
	SetVolume(val);
}
static void reset_volume(void) {
	SetVolume(SETTINGS_DEFAULT_VOLUME);
}

static int get_screen_timeout(void) {
	return (int)CFG_getScreenTimeoutSecs();
}
static void set_screen_timeout(int val) {
	CFG_setScreenTimeoutSecs((uint32_t)val);
}
static void reset_screen_timeout(void) {
	CFG_setScreenTimeoutSecs(CFG_DEFAULT_SCREENTIMEOUTSECS);
}

static int get_suspend_timeout(void) {
	return (int)CFG_getSuspendTimeoutSecs();
}
static void set_suspend_timeout(int val) {
	CFG_setSuspendTimeoutSecs((uint32_t)val);
}
static void reset_suspend_timeout(void) {
	CFG_setSuspendTimeoutSecs(CFG_DEFAULT_SUSPENDTIMEOUTSECS);
}

static int get_haptics(void) {
	return CFG_getHaptics() ? 1 : 0;
}
static void set_haptics(int v) {
	CFG_setHaptics(v != 0);
}
static void reset_haptics(void) {
	CFG_setHaptics(CFG_DEFAULT_HAPTICS);
}

static int get_default_view(void) {
	return CFG_getDefaultView();
}
static void set_default_view(int val) {
	CFG_setDefaultView(val);
}
static void reset_default_view(void) {
	CFG_setDefaultView(CFG_DEFAULT_VIEW);
}

static int get_clock24h(void) {
	return CFG_getClock24H() ? 1 : 0;
}
static void set_clock24h(int v) {
	CFG_setClock24H(v != 0);
}
static void reset_clock24h(void) {
	CFG_setClock24H(CFG_DEFAULT_CLOCK24H);
}

static int get_show_clock(void) {
	return CFG_getShowClock() ? 1 : 0;
}
static void set_show_clock(int v) {
	CFG_setShowClock(v != 0);
}
static void reset_show_clock(void) {
	CFG_setShowClock(CFG_DEFAULT_SHOWCLOCK);
}

static int get_ntp(void) {
	return TIME_getNetworkTimeSync() ? 1 : 0;
}
static void set_ntp(int v) {
	TIME_setNetworkTimeSync(v != 0);
}
static void reset_ntp(void) {
	TIME_setNetworkTimeSync(false);
}

static int get_timezone(void) {
	char* current = TIME_getCurrentTimezone();
	if (!current)
		return 0;
	int result = 0;
	for (int i = 0; i < tz_count; i++) {
		if (strcmp(timezones[i], current) == 0) {
			result = i;
			break;
		}
	}
	free(current);
	return result;
}
static void set_timezone(int idx) {
	if (idx >= 0 && idx < tz_count)
		TIME_setCurrentTimezone(timezones[idx]);
}
static void reset_timezone(void) {
	TIME_setCurrentTimezone("Asia/Shanghai");
}

static int get_save_format(void) {
	return CFG_getSaveFormat();
}
static void set_save_format(int val) {
	CFG_setSaveFormat(val);
}
static void reset_save_format(void) {
	CFG_setSaveFormat(CFG_DEFAULT_SAVEFORMAT);
}

static int get_state_format(void) {
	return CFG_getStateFormat();
}
static void set_state_format(int val) {
	CFG_setStateFormat(val);
}
static void reset_state_format(void) {
	CFG_setStateFormat(CFG_DEFAULT_STATEFORMAT);
}

static int get_use_extracted_filename(void) {
	return CFG_getUseExtractedFileName() ? 1 : 0;
}
static void set_use_extracted_filename(int v) {
	CFG_setUseExtractedFileName(v != 0);
}
static void reset_use_extracted_filename(void) {
	CFG_setUseExtractedFileName(CFG_DEFAULT_EXTRACTEDFILENAME);
}

static int get_power_off_protection(void) {
	return CFG_getPowerOffProtection() ? 1 : 0;
}
static void set_power_off_protection(int v) {
	CFG_setPowerOffProtection(v != 0);
}
static void reset_power_off_protection(void) {
	CFG_setPowerOffProtection(CFG_DEFAULT_POWEROFFPROTECTION);
}

static int get_fan_speed(void) {
	return GetFanSpeed();
}
static void set_fan_speed(int val) {
	SetFanSpeed(val);
}
static void reset_fan_speed(void) {
	SetFanSpeed(SETTINGS_DEFAULT_FAN_SPEED);
}

// ============================================
// FN Switch (Mute) callbacks
// ============================================

static int get_muted_volume(void) {
	return GetMutedVolume();
}
static void set_muted_volume(int val) {
	SetMutedVolume(val);
}
static void reset_muted_volume(void) {
	SetMutedVolume(0);
}

static int get_mute_leds(void) {
	return CFG_getMuteLEDs() ? 1 : 0;
}
static void set_mute_leds(int v) {
	CFG_setMuteLEDs(v != 0);
}
static void reset_mute_leds(void) {
	CFG_setMuteLEDs(CFG_DEFAULT_MUTELEDS);
}

static int get_muted_brightness(void) {
	return GetMutedBrightness();
}
static void set_muted_brightness(int val) {
	SetMutedBrightness(val);
}
static void reset_muted_brightness(void) {
	SetMutedBrightness(SETTINGS_DEFAULT_MUTE_NO_CHANGE);
}

static int get_muted_colortemp(void) {
	return GetMutedColortemp();
}
static void set_muted_colortemp(int val) {
	SetMutedColortemp(val);
}
static void reset_muted_colortemp(void) {
	SetMutedColortemp(SETTINGS_DEFAULT_MUTE_NO_CHANGE);
}

static int get_muted_contrast(void) {
	return GetMutedContrast();
}
static void set_muted_contrast(int val) {
	SetMutedContrast(val);
}
static void reset_muted_contrast(void) {
	SetMutedContrast(SETTINGS_DEFAULT_MUTE_NO_CHANGE);
}

static int get_muted_saturation(void) {
	return GetMutedSaturation();
}
static void set_muted_saturation(int val) {
	SetMutedSaturation(val);
}
static void reset_muted_saturation(void) {
	SetMutedSaturation(SETTINGS_DEFAULT_MUTE_NO_CHANGE);
}

static int get_muted_exposure(void) {
	return GetMutedExposure();
}
static void set_muted_exposure(int val) {
	SetMutedExposure(val);
}
static void reset_muted_exposure(void) {
	SetMutedExposure(SETTINGS_DEFAULT_MUTE_NO_CHANGE);
}

/* Turbo buttons */
static int get_turbo_a(void) {
	return GetMuteTurboA();
}
static void set_turbo_a(int v) {
	SetMuteTurboA(v);
}
static void reset_turbo_a(void) {
	SetMuteTurboA(0);
}

static int get_turbo_b(void) {
	return GetMuteTurboB();
}
static void set_turbo_b(int v) {
	SetMuteTurboB(v);
}
static void reset_turbo_b(void) {
	SetMuteTurboB(0);
}

static int get_turbo_x(void) {
	return GetMuteTurboX();
}
static void set_turbo_x(int v) {
	SetMuteTurboX(v);
}
static void reset_turbo_x(void) {
	SetMuteTurboX(0);
}

static int get_turbo_y(void) {
	return GetMuteTurboY();
}
static void set_turbo_y(int v) {
	SetMuteTurboY(v);
}
static void reset_turbo_y(void) {
	SetMuteTurboY(0);
}

static int get_turbo_l1(void) {
	return GetMuteTurboL1();
}
static void set_turbo_l1(int v) {
	SetMuteTurboL1(v);
}
static void reset_turbo_l1(void) {
	SetMuteTurboL1(0);
}

static int get_turbo_l2(void) {
	return GetMuteTurboL2();
}
static void set_turbo_l2(int v) {
	SetMuteTurboL2(v);
}
static void reset_turbo_l2(void) {
	SetMuteTurboL2(0);
}

static int get_turbo_r1(void) {
	return GetMuteTurboR1();
}
static void set_turbo_r1(int v) {
	SetMuteTurboR1(v);
}
static void reset_turbo_r1(void) {
	SetMuteTurboR1(0);
}

static int get_turbo_r2(void) {
	return GetMuteTurboR2();
}
static void set_turbo_r2(int v) {
	SetMuteTurboR2(v);
}
static void reset_turbo_r2(void) {
	SetMuteTurboR2(0);
}

/* Dpad mode when toggled */
static int get_mute_dpad_mode(void) {
	if (!GetMuteDisablesDpad() && !GetMuteEmulatesJoystick())
		return 0;
	if (GetMuteDisablesDpad() && GetMuteEmulatesJoystick())
		return 1;
	return 2;
}
static void set_mute_dpad_mode(int val) {
	SetMuteDisablesDpad(val == 1);
	SetMuteEmulatesJoystick(val > 0);
}
static void reset_mute_dpad_mode(void) {
	SetMuteDisablesDpad(0);
	SetMuteEmulatesJoystick(0);
}

// ============================================
// Notification callbacks
// ============================================

static int get_notify_save(void) {
	return CFG_getNotifyManualSave() ? 1 : 0;
}
static void set_notify_save(int v) {
	CFG_setNotifyManualSave(v != 0);
}
static void reset_notify_save(void) {
	CFG_setNotifyManualSave(CFG_DEFAULT_NOTIFY_MANUAL_SAVE);
}

static int get_notify_load(void) {
	return CFG_getNotifyLoad() ? 1 : 0;
}
static void set_notify_load(int v) {
	CFG_setNotifyLoad(v != 0);
}
static void reset_notify_load(void) {
	CFG_setNotifyLoad(CFG_DEFAULT_NOTIFY_LOAD);
}

static int get_notify_screenshot(void) {
	return CFG_getNotifyScreenshot() ? 1 : 0;
}
static void set_notify_screenshot(int v) {
	CFG_setNotifyScreenshot(v != 0);
}
static void reset_notify_screenshot(void) {
	CFG_setNotifyScreenshot(CFG_DEFAULT_NOTIFY_SCREENSHOT);
}

static int get_notify_adjustments(void) {
	return CFG_getNotifyAdjustments() ? 1 : 0;
}
static void set_notify_adjustments(int v) {
	CFG_setNotifyAdjustments(v != 0);
}
static void reset_notify_adjustments(void) {
	CFG_setNotifyAdjustments(CFG_DEFAULT_NOTIFY_ADJUSTMENTS);
}

static int get_notify_duration(void) {
	return CFG_getNotifyDuration();
}
static void set_notify_duration(int val) {
	CFG_setNotifyDuration(val);
}
static void reset_notify_duration(void) {
	CFG_setNotifyDuration(CFG_DEFAULT_NOTIFY_DURATION);
}

// ============================================
// RetroAchievements callbacks
// ============================================

static int get_ra_enable(void) {
	return CFG_getRAEnable() ? 1 : 0;
}
static void set_ra_enable(int v) {
	CFG_setRAEnable(v != 0);
}
static void reset_ra_enable(void) {
	CFG_setRAEnable(CFG_DEFAULT_RA_ENABLE);
}

static const char* get_ra_username_display(void) {
	const char* u = CFG_getRAUsername();
	return (u && u[0]) ? u : "(not set)";
}

static void on_ra_username_set(const char* text) {
	if (text)
		CFG_setRAUsername(text);
}

static const char* get_ra_password_display(void) {
	const char* p = CFG_getRAPassword();
	return (p && p[0]) ? "********" : "(not set)";
}

static void on_ra_password_set(const char* text) {
	if (text)
		CFG_setRAPassword(text);
}

/* RA authenticate button */
static char ra_auth_status_msg[256] = "";

static void on_ra_authenticate(void) {
	const char* username = CFG_getRAUsername();
	const char* password = CFG_getRAPassword();

	if (!username || !username[0] || !password || !password[0]) {
		snprintf(ra_auth_status_msg, sizeof(ra_auth_status_msg),
				 "Error: Username and password required");
		return;
	}

	RA_AuthResponse response;
	RA_AuthResult result = RA_authenticateSync(username, password, &response);

	if (result == RA_AUTH_SUCCESS) {
		CFG_setRAToken(response.token);
		CFG_setRAAuthenticated(true);
		snprintf(ra_auth_status_msg, sizeof(ra_auth_status_msg),
				 "Authenticated as %s", response.display_name);
	} else {
		CFG_setRAToken("");
		CFG_setRAAuthenticated(false);
		snprintf(ra_auth_status_msg, sizeof(ra_auth_status_msg),
				 "Error: %s", response.error_message);
	}
}

static const char* get_ra_status(void) {
	if (ra_auth_status_msg[0])
		return ra_auth_status_msg;
	if (CFG_getRAAuthenticated() && strlen(CFG_getRAToken()) > 0)
		return "Authenticated";
	return "Not authenticated";
}

static int get_ra_show_notifications(void) {
	return CFG_getRAShowNotifications() ? 1 : 0;
}
static void set_ra_show_notifications(int v) {
	CFG_setRAShowNotifications(v != 0);
}
static void reset_ra_show_notifications(void) {
	CFG_setRAShowNotifications(CFG_DEFAULT_RA_SHOW_NOTIFICATIONS);
}

static int get_ra_notify_duration(void) {
	return CFG_getRANotificationDuration();
}
static void set_ra_notify_duration(int val) {
	CFG_setRANotificationDuration(val);
}
static void reset_ra_notify_duration(void) {
	CFG_setRANotificationDuration(CFG_DEFAULT_RA_NOTIFICATION_DURATION);
}

static int get_ra_progress_duration(void) {
	return CFG_getRAProgressNotificationDuration();
}
static void set_ra_progress_duration(int val) {
	CFG_setRAProgressNotificationDuration(val);
}
static void reset_ra_progress_duration(void) {
	CFG_setRAProgressNotificationDuration(CFG_DEFAULT_RA_PROGRESS_NOTIFICATION_DURATION);
}

static int get_ra_sort_order(void) {
	return CFG_getRAAchievementSortOrder();
}
static void set_ra_sort_order(int val) {
	CFG_setRAAchievementSortOrder(val);
}
static void reset_ra_sort_order(void) {
	CFG_setRAAchievementSortOrder(CFG_DEFAULT_RA_ACHIEVEMENT_SORT_ORDER);
}

// ============================================
// About page static display callbacks
// ============================================

static char about_nextui_version[128] = "";
static char about_platform[128] = "";
static char about_os_version[128] = "";
static char about_busybox_version[128] = "";

static const char* get_about_version(void) {
	return about_nextui_version;
}
static const char* get_about_platform(void) {
	return about_platform;
}
static const char* get_about_os_version(void) {
	return about_os_version;
}
static const char* get_about_busybox(void) {
	return about_busybox_version;
}

static void init_about_info(void) {
	/* NextUI version: read from version.txt */
	FILE* vf = fopen(ROOT_SYSTEM_PATH "/version.txt", "r");
	if (vf) {
		if (fgets(about_nextui_version, sizeof(about_nextui_version), vf)) {
			/* strip trailing newline */
			int len = (int)strlen(about_nextui_version);
			while (len > 0 && (about_nextui_version[len - 1] == '\n' || about_nextui_version[len - 1] == '\r'))
				about_nextui_version[--len] = '\0';
		}
		fclose(vf);
	}

	/* Platform */
	char* model = PLAT_getModel();
	if (model) {
		strncpy(about_platform, model, sizeof(about_platform) - 1);
		about_platform[sizeof(about_platform) - 1] = '\0';
	}

	/* Stock OS version */
	PLAT_getOsVersionInfo(about_os_version, sizeof(about_os_version));

	/* BusyBox version */
	char bb_output[512];
	if (exec_command("cat --help", bb_output, sizeof(bb_output)) == 0) {
		extract_busybox_version(bb_output, about_busybox_version, sizeof(about_busybox_version));
	}
	if (!about_busybox_version[0]) {
		strncpy(about_busybox_version, "BusyBox version not found.", sizeof(about_busybox_version) - 1);
	}
}

// ============================================
// Menu item arrays (static allocation)
// ============================================

#define MAX_APPEARANCE_ITEMS 20
#define MAX_DISPLAY_ITEMS 8
#define MAX_SYSTEM_ITEMS 18
#define MAX_MUTE_ITEMS 20
#define MAX_NOTIFY_ITEMS 8
#define MAX_RA_ITEMS 15
#define MAX_ABOUT_ITEMS 8
#define MAX_MAIN_ITEMS 10

static SettingItem appearance_items[MAX_APPEARANCE_ITEMS];
static SettingItem display_items[MAX_DISPLAY_ITEMS];
static SettingItem system_items[MAX_SYSTEM_ITEMS];
static SettingItem mute_items[MAX_MUTE_ITEMS];
static SettingItem notify_items[MAX_NOTIFY_ITEMS];
static SettingItem ra_items[MAX_RA_ITEMS];
static SettingItem about_items[MAX_ABOUT_ITEMS];
static SettingItem main_items[MAX_MAIN_ITEMS];

static SettingsPage appearance_page;
static SettingsPage display_page;
static SettingsPage system_page;
static SettingsPage mute_page;
static SettingsPage fn_switch_page; /* wraps mute_items into "FN Switch" titled page */
static SettingsPage notify_page;
static SettingsPage ra_page;
static SettingsPage about_page;
static SettingsPage main_page;

/* WiFi/BT pages (created upfront in build_menu_tree) */
static SettingsPage* wifi_page_ptr = NULL;
static SettingsPage* bt_page_ptr = NULL;

// ============================================
// Reset button callbacks (reference pages)
// ============================================

static void reset_appearance_page(void) {
	settings_page_reset_all(&appearance_page);
}
static void reset_display_page(void) {
	settings_page_reset_all(&display_page);
}
static void reset_system_page(void) {
	settings_page_reset_all(&system_page);
}
static void reset_mute_page(void) {
	settings_page_reset_all(&fn_switch_page);
}
static void reset_notify_page(void) {
	settings_page_reset_all(&notify_page);
}
static void reset_ra_page(void) {
	settings_page_reset_all(&ra_page);
}

// ============================================
// Helper macro for item initialization
// ============================================

#define ITEM_CYCLE_INIT(_name, _desc, _labels, _lcount, _values, _get, _set, _reset) \
	{.name = (_name), .desc = (_desc), .type = ITEM_CYCLE, .visible = 1, .labels = (_labels), .label_count = (_lcount), .current_idx = 0, .get_value = (_get), .set_value = (_set), .values = (_values), .on_press = NULL, .submenu = NULL, .display_text = {0}, .get_display = NULL, .text_value = {0}, .on_text_set = NULL, .get_text = NULL, .on_reset = (_reset), .custom_draw = NULL, .user_data = NULL}

#define ITEM_COLOR_INIT(_name, _desc, _labels, _lcount, _values, _get, _set, _reset) \
	{.name = (_name), .desc = (_desc), .type = ITEM_COLOR, .visible = 1, .labels = (_labels), .label_count = (_lcount), .current_idx = 0, .get_value = (_get), .set_value = (_set), .values = (_values), .on_press = NULL, .submenu = NULL, .display_text = {0}, .get_display = NULL, .text_value = {0}, .on_text_set = NULL, .get_text = NULL, .on_reset = (_reset), .custom_draw = NULL, .user_data = NULL}

#define ITEM_BUTTON_INIT(_name, _desc, _on_press) \
	{.name = (_name), .desc = (_desc), .type = ITEM_BUTTON, .visible = 1, .labels = NULL, .label_count = 0, .current_idx = 0, .get_value = NULL, .set_value = NULL, .values = NULL, .on_press = (_on_press), .submenu = NULL, .display_text = {0}, .get_display = NULL, .text_value = {0}, .on_text_set = NULL, .get_text = NULL, .on_reset = NULL, .custom_draw = NULL, .user_data = NULL}

#define ITEM_SUBMENU_INIT(_name, _desc, _submenu) \
	{.name = (_name), .desc = (_desc), .type = ITEM_SUBMENU, .visible = 1, .labels = NULL, .label_count = 0, .current_idx = 0, .get_value = NULL, .set_value = NULL, .values = NULL, .on_press = NULL, .submenu = (_submenu), .display_text = {0}, .get_display = NULL, .text_value = {0}, .on_text_set = NULL, .get_text = NULL, .on_reset = NULL, .custom_draw = NULL, .user_data = NULL}

#define ITEM_STATIC_INIT(_name, _desc, _get_display) \
	{.name = (_name), .desc = (_desc), .type = ITEM_STATIC, .visible = 1, .labels = NULL, .label_count = 0, .current_idx = 0, .get_value = NULL, .set_value = NULL, .values = NULL, .on_press = NULL, .submenu = NULL, .display_text = {0}, .get_display = (_get_display), .text_value = {0}, .on_text_set = NULL, .get_text = NULL, .on_reset = NULL, .custom_draw = NULL, .user_data = NULL}

#define ITEM_TEXT_INPUT_INIT(_name, _desc, _get_text, _on_text_set) \
	{.name = (_name), .desc = (_desc), .type = ITEM_TEXT_INPUT, .visible = 1, .labels = NULL, .label_count = 0, .current_idx = 0, .get_value = NULL, .set_value = NULL, .values = NULL, .on_press = NULL, .submenu = NULL, .display_text = {0}, .get_display = NULL, .text_value = {0}, .on_text_set = (_on_text_set), .get_text = (_get_text), .on_reset = NULL, .custom_draw = NULL, .user_data = NULL}

// ============================================
// Build menu tree
// ============================================

static void init_page(SettingsPage* page, const char* title, SettingItem* items, int count, int is_list) {
	memset(page, 0, sizeof(SettingsPage));
	page->title = title;
	page->items = items;
	page->item_count = count;
	page->selected = 0;
	page->scroll = 0;
	page->is_list = is_list;
	page->on_show = NULL;
	page->on_hide = NULL;
	page->on_tick = NULL;
	page->dynamic_start = -1;
	page->max_items = count;
	page->needs_layout = 0;
}

static void build_menu_tree(const DeviceInfo* dev) {
	int idx;

	// ============================
	// Appearance page
	// ============================
	idx = 0;
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Font", "The font to render all UI text.",
		font_labels, 2, NULL, get_font, set_font, reset_font);
	appearance_items[idx++] = (SettingItem)ITEM_COLOR_INIT(
		"Main Color", "The color used to render main UI elements.",
		color_labels, COLOR_COUNT, (int*)color_values, get_color1, set_color1, reset_color1);
	appearance_items[idx++] = (SettingItem)ITEM_COLOR_INIT(
		"Primary Accent Color", "The color used to highlight important things in the user interface.",
		color_labels, COLOR_COUNT, (int*)color_values, get_color2, set_color2, reset_color2);
	appearance_items[idx++] = (SettingItem)ITEM_COLOR_INIT(
		"Secondary Accent Color", "A secondary highlight color.",
		color_labels, COLOR_COUNT, (int*)color_values, get_color3, set_color3, reset_color3);
	appearance_items[idx++] = (SettingItem)ITEM_COLOR_INIT(
		"Hint info Color", "Color for button hints and info",
		color_labels, COLOR_COUNT, (int*)color_values, get_color6, set_color6, reset_color6);
	appearance_items[idx++] = (SettingItem)ITEM_COLOR_INIT(
		"List Text", "List text color",
		color_labels, COLOR_COUNT, (int*)color_values, get_color4, set_color4, reset_color4);
	appearance_items[idx++] = (SettingItem)ITEM_COLOR_INIT(
		"List Text Selected", "List selected text color",
		color_labels, COLOR_COUNT, (int*)color_values, get_color5, set_color5, reset_color5);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show battery percentage", "Show battery level as percent in the status pill",
		on_off_labels, 2, on_off_values, get_show_battery_percent, set_show_battery_percent, reset_show_battery_percent);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show menu animations", "Enable or disable menu animations",
		on_off_labels, 2, on_off_values, get_menu_animations, set_menu_animations, reset_menu_animations);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show menu transitions", "Enable or disable animated transitions",
		on_off_labels, 2, on_off_values, get_menu_transitions, set_menu_transitions, reset_menu_transitions);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Game art corner radius", "Set the radius for the rounded corners of game art",
		thumb_radius_labels, THUMB_RADIUS_LABEL_COUNT, NULL, get_thumb_radius, set_thumb_radius, reset_thumb_radius);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Game art width", "Set the percentage of screen width used for game art.",
		game_art_width_labels, GAME_ART_WIDTH_COUNT, game_art_width_values, get_game_art_width, set_game_art_width, reset_game_art_width);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show folder names at root", "Show folder names at root directory",
		on_off_labels, 2, on_off_values, get_show_folder_names, set_show_folder_names, reset_show_folder_names);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show Recents", "Show \"Recently Played\" menu entry in game list.",
		on_off_labels, 2, on_off_values, get_show_recents, set_show_recents, reset_show_recents);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show Tools", "Show \"Tools\" menu entry in game list.",
		on_off_labels, 2, on_off_values, get_show_tools, set_show_tools, reset_show_tools);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show game art", "Show game artwork in the main menu",
		on_off_labels, 2, on_off_values, get_show_game_art, set_show_game_art, reset_show_game_art);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Use folder background for ROMs", "If enabled, used the emulator background image.",
		on_off_labels, 2, on_off_values, get_roms_use_folder_bg, set_roms_use_folder_bg, reset_roms_use_folder_bg);
	appearance_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show Quickswitcher UI", "Show/hide Quickswitcher UI elements.",
		on_off_labels, 2, on_off_values, get_show_quickswitcher, set_show_quickswitcher, reset_show_quickswitcher);
	appearance_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Reset to defaults", "Resets all options in this menu to their default values.",
		reset_appearance_page);
	init_page(&appearance_page, "Appearance", appearance_items, idx, 0);

	// ============================
	// Display page
	// ============================
	idx = 0;
	display_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Brightness", "Display brightness (0 to 10)",
		brightness_labels, BRIGHTNESS_LABEL_COUNT, NULL, get_brightness, set_brightness, reset_brightness);

	if (has_color_temp(dev)) {
		display_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Color temperature", "Color temperature (0 to 40)",
			colortemp_labels, COLORTEMP_LABEL_COUNT, NULL, get_colortemp, set_colortemp, reset_colortemp);
	}

	if (has_contrast_sat(dev)) {
		display_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Contrast", "Contrast enhancement (-4 to 5)",
			contrast_labels, CONTRAST_LABEL_COUNT, contrast_values, get_contrast, set_contrast, reset_contrast);
		display_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Saturation", "Saturation enhancement (-5 to 5)",
			saturation_labels, SATURATION_LABEL_COUNT, saturation_values, get_saturation, set_saturation, reset_saturation);
	}

	if (has_exposure(dev)) {
		display_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Exposure", "Exposure enhancement (-4 to 5)",
			exposure_labels, EXPOSURE_LABEL_COUNT, exposure_values, get_exposure, set_exposure, reset_exposure);
	}

	display_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Reset to defaults", "Resets all options in this menu to their default values.",
		reset_display_page);
	init_page(&display_page, "Display", display_items, idx, 0);

	// ============================
	// System page
	// ============================
	idx = 0;
	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Volume", "Speaker volume",
		volume_labels, VOLUME_LABEL_COUNT, volume_values, get_volume, set_volume, reset_volume);
	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Screen timeout", "Period of inactivity before screen turns off (0-600s)",
		screen_timeout_labels, SCREEN_TIMEOUT_COUNT, screen_timeout_values, get_screen_timeout, set_screen_timeout, reset_screen_timeout);
	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Suspend timeout", "Time before device goes to sleep after screen is off (5-600s)",
		sleep_timeout_labels, SLEEP_TIMEOUT_COUNT, sleep_timeout_values, get_suspend_timeout, set_suspend_timeout, reset_suspend_timeout);
	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Haptic feedback", "Enable or disable haptic feedback on certain actions in the OS",
		on_off_labels, 2, on_off_values, get_haptics, set_haptics, reset_haptics);
	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Default view", "The initial view to show on boot",
		default_view_labels, DEFAULT_VIEW_COUNT, default_view_values, get_default_view, set_default_view, reset_default_view);
	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show 24h time format", "Show clock in the 24hrs time format",
		on_off_labels, 2, on_off_values, get_clock24h, set_clock24h, reset_clock24h);
	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show clock", "Show clock in the status pill",
		on_off_labels, 2, on_off_values, get_show_clock, set_show_clock, reset_show_clock);
	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Set time and date automatically", "Sync time via NTP (requires internet)",
		on_off_labels, 2, on_off_values, get_ntp, set_ntp, reset_ntp);

	if (tz_count > 0) {
		system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Time zone", "Your time zone",
			tz_labels, tz_count, tz_values, get_timezone, set_timezone, reset_timezone);
	}

	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Save format", "The save format to use.",
		save_format_labels, SAVE_FORMAT_COUNT, save_format_values, get_save_format, set_save_format, reset_save_format);
	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Save state format", "The save state format to use.",
		state_format_labels, STATE_FORMAT_COUNT, state_format_values, get_state_format, set_state_format, reset_state_format);
	system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Use extracted file name", "Use the extracted file name instead of the archive name.",
		on_off_labels, 2, on_off_values, get_use_extracted_filename, set_use_extracted_filename, reset_use_extracted_filename);

	if (dev->platform == PLAT_TG5040) {
		system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Safe poweroff", "Bypasses the stock shutdown procedure to avoid the \"limbo bug\".",
			on_off_labels, 2, on_off_values, get_power_off_protection, set_power_off_protection, reset_power_off_protection);
	}

	if (has_active_cooling(dev)) {
		system_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Fan Speed", "Select the fan speed percentage (Quiet/Normal/Performance or 0-100%)",
			fan_speed_labels, FAN_SPEED_COUNT, fan_speed_values, get_fan_speed, set_fan_speed, reset_fan_speed);
	}

	system_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Reset to defaults", "Resets all options in this menu to their default values.",
		reset_system_page);
	init_page(&system_page, "System", system_items, idx, 0);

	// ============================
	// FN Switch (Mute) page
	// ============================
	idx = 0;
	mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Volume when toggled", "Speaker volume (0-20)",
		mute_volume_labels, MUTE_VOLUME_COUNT, mute_volume_values, get_muted_volume, set_muted_volume, reset_muted_volume);
	mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"FN switch disables LED", "Switch will also disable LEDs",
		on_off_labels, 2, on_off_values, get_mute_leds, set_mute_leds, reset_mute_leds);
	mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Brightness when toggled", "Display brightness (0 to 10)",
		mute_brightness_labels, MUTE_BRIGHTNESS_COUNT, mute_brightness_values, get_muted_brightness, set_muted_brightness, reset_muted_brightness);

	if (has_mute_toggle(dev)) {
		if (has_color_temp(dev)) {
			mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
				"Color temperature when toggled", "Color temperature (0 to 40)",
				mute_colortemp_labels, MUTE_COLORTEMP_COUNT, mute_colortemp_values, get_muted_colortemp, set_muted_colortemp, reset_muted_colortemp);
		}
		if (has_contrast_sat(dev)) {
			mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
				"Contrast when toggled", "Contrast enhancement (-4 to 5)",
				mute_contrast_labels, MUTE_CONTRAST_COUNT, mute_contrast_values, get_muted_contrast, set_muted_contrast, reset_muted_contrast);
			mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
				"Saturation when toggled", "Saturation enhancement (-5 to 5)",
				mute_saturation_labels, MUTE_SATURATION_COUNT, mute_saturation_values, get_muted_saturation, set_muted_saturation, reset_muted_saturation);
		}
		if (has_exposure(dev)) {
			mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
				"Exposure when toggled", "Exposure enhancement (-4 to 5)",
				mute_exposure_labels, MUTE_EXPOSURE_COUNT, mute_exposure_values, get_muted_exposure, set_muted_exposure, reset_muted_exposure);
		}

		/* Turbo buttons */
		mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Turbo fire A", "Enable turbo fire A",
			on_off_labels, 2, on_off_values, get_turbo_a, set_turbo_a, reset_turbo_a);
		mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Turbo fire B", "Enable turbo fire B",
			on_off_labels, 2, on_off_values, get_turbo_b, set_turbo_b, reset_turbo_b);
		mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Turbo fire X", "Enable turbo fire X",
			on_off_labels, 2, on_off_values, get_turbo_x, set_turbo_x, reset_turbo_x);
		mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Turbo fire Y", "Enable turbo fire Y",
			on_off_labels, 2, on_off_values, get_turbo_y, set_turbo_y, reset_turbo_y);
		mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Turbo fire L1", "Enable turbo fire L1",
			on_off_labels, 2, on_off_values, get_turbo_l1, set_turbo_l1, reset_turbo_l1);
		mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Turbo fire L2", "Enable turbo fire L2",
			on_off_labels, 2, on_off_values, get_turbo_l2, set_turbo_l2, reset_turbo_l2);
		mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Turbo fire R1", "Enable turbo fire R1",
			on_off_labels, 2, on_off_values, get_turbo_r1, set_turbo_r1, reset_turbo_r1);
		mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Turbo fire R2", "Enable turbo fire R2",
			on_off_labels, 2, on_off_values, get_turbo_r2, set_turbo_r2, reset_turbo_r2);
	}

	if (has_mute_toggle(dev) && has_analog_sticks(dev)) {
		mute_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
			"Dpad mode when toggled", "Dpad: default. Joystick: Dpad exclusively acts as analog stick.\nBoth: Dpad and Joystick inputs at the same time.",
			dpad_mode_labels, DPAD_MODE_COUNT, dpad_mode_values, get_mute_dpad_mode, set_mute_dpad_mode, reset_mute_dpad_mode);
	}

	mute_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Reset to defaults", "Resets all options in this menu to their default values.",
		reset_mute_page);
	init_page(&fn_switch_page, "FN Switch", mute_items, idx, 0);

	// ============================
	// Notifications page
	// ============================
	idx = 0;
	notify_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Save states", "Show notification when saving game state",
		on_off_labels, 2, on_off_values, get_notify_save, set_notify_save, reset_notify_save);
	notify_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Load states", "Show notification when loading game state",
		on_off_labels, 2, on_off_values, get_notify_load, set_notify_load, reset_notify_load);
	notify_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Screenshots", "Show notification when taking a screenshot",
		on_off_labels, 2, on_off_values, get_notify_screenshot, set_notify_screenshot, reset_notify_screenshot);
	notify_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Vol / Display Adjustments", "Show overlay for volume, brightness, and color temp adjustments",
		on_off_labels, 2, on_off_values, get_notify_adjustments, set_notify_adjustments, reset_notify_adjustments);
	notify_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Duration", "How long notifications stay on screen",
		notify_duration_labels, NOTIFY_DURATION_COUNT, notify_duration_values, get_notify_duration, set_notify_duration, reset_notify_duration);
	notify_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Reset to defaults", "Resets all options in this menu to their default values.",
		reset_notify_page);
	init_page(&notify_page, "Notifications", notify_items, idx, 0);

	// ============================
	// RetroAchievements page
	// ============================
	idx = 0;
	ra_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Enable Achievements", "Enable RetroAchievements integration",
		on_off_labels, 2, on_off_values, get_ra_enable, set_ra_enable, reset_ra_enable);
	ra_items[idx++] = (SettingItem)ITEM_TEXT_INPUT_INIT(
		"Username", "RetroAchievements username",
		get_ra_username_display, on_ra_username_set);
	ra_items[idx++] = (SettingItem)ITEM_TEXT_INPUT_INIT(
		"Password", "RetroAchievements password",
		get_ra_password_display, on_ra_password_set);
	ra_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Authenticate", "Test credentials and retrieve API token",
		on_ra_authenticate);
	ra_items[idx++] = (SettingItem)ITEM_STATIC_INIT(
		"Status", "Authentication status",
		get_ra_status);
	ra_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Show Notifications", "Show achievement unlock notifications",
		on_off_labels, 2, on_off_values, get_ra_show_notifications, set_ra_show_notifications, reset_ra_show_notifications);
	ra_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Notification Duration", "How long achievement notifications stay on screen",
		notify_duration_labels, NOTIFY_DURATION_COUNT, notify_duration_values, get_ra_notify_duration, set_ra_notify_duration, reset_ra_notify_duration);
	ra_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Progress Duration", "Duration for progress updates (top-left). Off to disable.",
		progress_duration_labels, PROGRESS_DURATION_COUNT, progress_duration_values, get_ra_progress_duration, set_ra_progress_duration, reset_ra_progress_duration);
	ra_items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Achievement Sort Order", "How achievements are sorted in the in-game menu",
		ra_sort_labels, RA_SORT_LABEL_COUNT, ra_sort_values, get_ra_sort_order, set_ra_sort_order, reset_ra_sort_order);
	ra_items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Reset to defaults", "Resets all options in this menu to their default values.",
		reset_ra_page);
	init_page(&ra_page, "RetroAchievements", ra_items, idx, 0);

	// ============================
	// About page
	// ============================
	idx = 0;
	about_items[idx++] = (SettingItem)ITEM_STATIC_INIT(
		"NextUI version", "", get_about_version);
	about_items[idx++] = (SettingItem)ITEM_STATIC_INIT(
		"Platform", "", get_about_platform);
	about_items[idx++] = (SettingItem)ITEM_STATIC_INIT(
		"Stock OS version", "", get_about_os_version);
	about_items[idx++] = (SettingItem)ITEM_STATIC_INIT(
		"Busybox version", "", get_about_busybox);
	init_page(&about_page, "About", about_items, idx, 0);

	// ============================
	// Main page (category list)
	// ============================
	idx = 0;
	main_items[idx++] = (SettingItem)ITEM_SUBMENU_INIT(
		"Appearance", "UI customization", &appearance_page);
	main_items[idx++] = (SettingItem)ITEM_SUBMENU_INIT(
		"Display", "", &display_page);
	main_items[idx++] = (SettingItem)ITEM_SUBMENU_INIT(
		"System", "", &system_page);

	if (has_mute_toggle(dev)) {
		main_items[idx++] = (SettingItem)ITEM_SUBMENU_INIT(
			"FN switch", "FN switch settings", &fn_switch_page);
	}

	main_items[idx++] = (SettingItem)ITEM_SUBMENU_INIT(
		"Notifications", "Save state notifications", &notify_page);
	main_items[idx++] = (SettingItem)ITEM_SUBMENU_INIT(
		"RetroAchievements", "Achievement tracking settings", &ra_page);

	if (has_wifi(dev)) {
		wifi_page_ptr = wifi_page_create();
		if (wifi_page_ptr) {
			main_items[idx++] = (SettingItem)ITEM_SUBMENU_INIT("Network", "", wifi_page_ptr);
		}
	}

	if (has_bluetooth(dev)) {
		bt_page_ptr = bt_page_create();
		if (bt_page_ptr) {
			main_items[idx++] = (SettingItem)ITEM_SUBMENU_INIT("Bluetooth", "", bt_page_ptr);
		}
	}

	main_items[idx++] = (SettingItem)ITEM_SUBMENU_INIT(
		"About", "", &about_page);

	init_page(&main_page, "Settings", main_items, idx, 1);

	// ============================
	// Sync all CYCLE items
	// ============================
	{
		SettingsPage* pages[] = {
			&appearance_page, &display_page, &system_page,
			&fn_switch_page, &notify_page, &ra_page, NULL};
		for (int p = 0; pages[p]; p++) {
			for (int i = 0; i < pages[p]->item_count; i++) {
				settings_item_sync(&pages[p]->items[i]);
			}
		}
	}
}

// ============================================
// main()
// ============================================

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	// Initialize display and show splash screen ASAP
	SDL_Surface* screen = GFX_init(MODE_MAIN);
	UI_showSplashScreen(screen, "Settings");

	// Now do the slower initialization
	DeviceInfo dev = device_detect();

	char version[128];
	PLAT_getOsVersionInfo(version, 128);
	LOG_info("This is stock OS version %s\n", version);

	InitSettings();
	PWR_init();
	PAD_init();
	TIME_init();

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	/* Generate dynamic label arrays */
	init_dynamic_labels();

	/* Initialize about info */
	init_about_info();

	/* Build menu tree */
	build_menu_tree(&dev);

	/* Set screen pointer on WiFi/BT pages for overlay rendering */
	if (wifi_page_ptr)
		wifi_page_ptr->screen = screen;
	if (bt_page_ptr)
		bt_page_ptr->screen = screen;

	settings_menu_init();
	settings_menu_push(&main_page);

	int dirty = 1;
	int show_setting = 0;
	int was_online = PWR_isOnline();
	int had_bt = PLAT_btIsConnected();
	int quit = 0;
	uint32_t start_press_time = 0;

	while (!quit && !app_quit) {
		GFX_startFrame();
		PAD_poll();

		// Long-press START to exit with confirmation
		if (PAD_justPressed(BTN_START))
			start_press_time = SDL_GetTicks();
		if (PAD_isPressed(BTN_START) && start_press_time &&
			SDL_GetTicks() - start_press_time >= 500) {
			start_press_time = 0;
			PAD_reset();

			// Show confirmation dialog
			int confirmed = 0;
			int dialog_done = 0;
			while (!dialog_done) {
				GFX_startFrame();
				PAD_poll();
				if (PAD_justPressed(BTN_A)) {
					confirmed = 1;
					dialog_done = 1;
				} else if (PAD_justPressed(BTN_B)) {
					dialog_done = 1;
				}
				UI_renderConfirmDialog(screen, "Exit Settings?",
									   "Your settings are automatically saved");
				GFX_flip(screen);
			}
			PAD_reset();
			if (confirmed) {
				quit = 1;
				continue;
			}
			dirty = 1;
			continue;
		}
		if (!PAD_isPressed(BTN_START))
			start_press_time = 0;

		quit = settings_menu_handle_input(&dirty);
		PWR_update((bool*)&dirty, &show_setting, NULL, NULL);

		int is_online = PWR_isOnline();
		if (was_online != is_online)
			dirty = 1;
		was_online = is_online;

		int has_bt = PLAT_btIsConnected();
		if (had_bt != has_bt)
			dirty = 1;
		had_bt = has_bt;

		if (dirty) {
			GFX_clear(screen);
			settings_menu_render(screen, show_setting);
			GFX_flip(screen);
			dirty = 0;
		} else {
			GFX_sync();
		}
	}

	/* Clear screen to black to avoid visual artifacts on exit */
	GFX_clear(screen);
	GFX_flip(screen);

	/* Cleanup */
	if (wifi_page_ptr)
		wifi_page_destroy(wifi_page_ptr);
	if (bt_page_ptr)
		bt_page_destroy(bt_page_ptr);
	free_dynamic_labels();

	QuitSettings();
	PWR_quit();
	PAD_quit();
	BT_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
