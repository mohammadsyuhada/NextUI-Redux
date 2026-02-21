#ifndef SETTINGS_MENU_H
#define SETTINGS_MENU_H

#include <pthread.h>
#include <stdbool.h>
#include "sdl.h"
#include "api.h"

// Forward declarations
struct SettingItem;
struct SettingsPage;

// ============================================
// Device Platform (shared across settings modules)
// ============================================

typedef enum {
	PLAT_UNKNOWN = 0,
	PLAT_TG5040,
	PLAT_TG5050,
	PLAT_MY355
} DevicePlatform;

// ============================================
// Item Types
// ============================================

typedef enum {
	ITEM_CYCLE,		 // left/right cycles through value labels
	ITEM_COLOR,		 // like CYCLE but also draws a color swatch
	ITEM_BUTTON,	 // A to press (e.g., "Reset to defaults")
	ITEM_SUBMENU,	 // A opens sub-page
	ITEM_STATIC,	 // display-only (e.g., About version info)
	ITEM_TEXT_INPUT, // A opens UIKeyboard_open()
} ItemType;

// ============================================
// Setting Item
// ============================================

typedef struct SettingItem {
	const char* name;
	const char* desc; // description shown at bottom when selected
	ItemType type;
	int visible; // 1=shown, 0=hidden (for device-conditional items)

	// ITEM_CYCLE
	const char** labels; // array of display strings
	int label_count;
	int current_idx;			// current selection index
	int (*get_value)(void);		// returns current value
	void (*set_value)(int val); // sets value
	int* values;				// maps idx -> actual value (NULL means idx=value)

	// ITEM_BUTTON
	void (*on_press)(void);

	// ITEM_SUBMENU
	struct SettingsPage* submenu;

	// ITEM_STATIC
	char display_text[128];
	const char* (*get_display)(void); // dynamic display text getter

	// ITEM_TEXT_INPUT
	char text_value[128];
	void (*on_text_set)(const char* text);
	const char* (*get_text)(void); // returns current text value for display

	// Reset (shared by CYCLE items in a group)
	void (*on_reset)(void);

	// Custom draw (for WiFi/BT items with icons)
	void (*custom_draw)(SDL_Surface* screen, struct SettingItem* item,
						int x, int y, int w, int h, int selected);
	void* user_data; // for custom items (network/device info)
} SettingItem;

// ============================================
// Settings Page
// ============================================

typedef struct SettingsPage {
	const char* title;
	SettingItem* items; // array of items
	int item_count;
	int selected;
	int scroll;
	int is_list; // 1=category list (shrink pills), 0=settings (full-width pills)

	// Lifecycle callbacks (for WiFi/BT pages)
	void (*on_show)(struct SettingsPage* page);
	void (*on_hide)(struct SettingsPage* page);
	void (*on_tick)(struct SettingsPage* page); // called each frame for dynamic updates

	// Dynamic items (WiFi/BT)
	int dynamic_start;		// index where dynamic items begin (-1 if none)
	int max_items;			// allocated capacity of items array
	pthread_rwlock_t lock;	// reader/writer lock for thread safety
	int needs_layout;		// flag: scanner thread updated items
	int input_blocked;		// flag: block value cycling (e.g. async toggle in progress)
	const char* status_msg; // transient message rendered below items (e.g. "Scanning...")
	SDL_Surface* screen;	// screen surface for overlay rendering (set by main app)
} SettingsPage;

// ============================================
// Menu System API
// ============================================

// Page stack management
void settings_menu_init(void);
void settings_menu_push(SettingsPage* page);
void settings_menu_pop(void);
SettingsPage* settings_menu_current(void);
int settings_menu_depth(void);

void settings_menu_handle_input(bool* quit, bool* dirty);

// Render the current page
void settings_menu_render(SDL_Surface* screen, IndicatorType show_setting);

// Sync a CYCLE item's current_idx from its get_value callback
void settings_item_sync(SettingItem* item);

// Get the visible item count for a page
int settings_page_visible_count(SettingsPage* page);

// Get visible item by visible index (skipping hidden items)
SettingItem* settings_page_visible_item(SettingsPage* page, int visible_idx);

// Map visible index to actual index in items array
int settings_page_visible_to_actual(SettingsPage* page, int visible_idx);

// Map actual index to visible index
int settings_page_actual_to_visible(SettingsPage* page, int actual_idx);

// Reset all items in a page to defaults
void settings_page_reset_all(SettingsPage* page);

// Initialize a page's rwlock (call for pages with dynamic items)
void settings_page_init_lock(SettingsPage* page);

// Clean up a page
void settings_page_destroy(SettingsPage* page);

// ============================================
// Helper macros for item initialization
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

#endif // SETTINGS_MENU_H
