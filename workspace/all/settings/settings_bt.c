#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "settings_bt.h"
#include "defines.h"
#include "api.h"
#include "ui_list.h"
#include "ui_components.h"

#ifdef HAS_BTAGENT
#include "settings_btagent.h"
#endif

// ============================================
// BT Device Info (attached to user_data)
// ============================================

typedef struct {
	char name[249];
	char addr[18];
	BluetoothDeviceType device_type;
	int paired;
	int connected;
	int16_t rssi;
} BtDeviceInfo;

// ============================================
// Static items
// ============================================

#define BT_STATIC_COUNT 3
#define BT_IDX_TOGGLE 0
#define BT_IDX_DIAG 1
#define BT_IDX_RATE 2

static const char* bt_toggle_labels[] = {"Off", "On"};
static const char* bt_diag_labels[] = {"Off", "On"};
static const char* bt_rate_labels[] = {"44100 Hz", "48000 Hz"};
static int bt_rate_values[] = {44100, 48000};

// ============================================
// Scanner thread state
// ============================================

static pthread_t bt_scanner_thread;
static volatile int bt_scanner_running = 0;
static int bt_scanner_started = 0;
static SettingsPage* bt_page_ref = NULL;

// ============================================
// BT device options submenu
// ============================================

typedef struct {
	SettingsPage page;
	SettingItem items[4];
	int item_count;
	BtDeviceInfo dev_info;
} BtDeviceOptions;

// Forward declarations
static void bt_on_show(SettingsPage* page);
static void bt_on_hide(SettingsPage* page);
static void bt_on_tick(SettingsPage* page);
static void bt_device_draw(SDL_Surface* screen, SettingItem* item,
						   int x, int y, int w, int h, int selected);

// ============================================
// BT toggle (blocking with overlay)
// ============================================

static void* bt_toggle_thread(void* arg) {
	struct {
		int val;
		volatile int* done;
	}* ctx = arg;
	BT_enable(ctx->val ? true : false);
	*ctx->done = 1;
	return NULL;
}

static int bt_get_toggle(void) {
	return BT_enabled() ? 1 : 0;
}

static void bt_set_toggle(int val) {
	SettingsPage* page = settings_menu_current();
	if (!page || !page->screen)
		return;

	volatile int done = 0;
	struct {
		int val;
		volatile int* done;
	} ctx = {val, &done};

	pthread_t t;
	pthread_create(&t, NULL, bt_toggle_thread, &ctx);
	pthread_detach(t);

	const char* title = val ? "Enabling Bluetooth..." : "Disabling Bluetooth...";

	while (!done) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_B))
			break;

		GFX_clear(page->screen);
		settings_menu_render(page->screen, 0);
		UI_renderLoadingOverlay(page->screen, title, "Press B to cancel");
		GFX_flip(page->screen);
	}

	// Re-sync with actual hardware state
	settings_item_sync(&page->items[BT_IDX_TOGGLE]);
}

static int bt_get_diag(void) {
	return BT_diagnosticsEnabled() ? 1 : 0;
}

static void bt_set_diag(int val) {
	BT_diagnosticsEnable(val ? true : false);
}

static int bt_get_rate(void) {
	return CFG_getBluetoothSamplingrateLimit();
}

static void bt_set_rate(int val) {
	CFG_setBluetoothSamplingrateLimit(val);
}

static void bt_reset_rate(void) {
	CFG_setBluetoothSamplingrateLimit(CFG_DEFAULT_BLUETOOTH_MAXRATE);
}

// ============================================
// Device action callbacks
// ============================================

static BtDeviceOptions* active_bt_options = NULL;

static void bt_action_connect(void) {
	if (!active_bt_options)
		return;
	BT_connect(active_bt_options->dev_info.addr);
	settings_menu_pop();
}

static void bt_action_disconnect(void) {
	if (!active_bt_options)
		return;
	BT_disconnect(active_bt_options->dev_info.addr);
	settings_menu_pop();
}

static void bt_action_pair(void) {
	if (!active_bt_options)
		return;
	BT_pair(active_bt_options->dev_info.addr);
	settings_menu_pop();
}

static void bt_action_unpair(void) {
	if (!active_bt_options)
		return;
	BT_unpair(active_bt_options->dev_info.addr);
	settings_menu_pop();
}

// ============================================
// Build device options submenu
// ============================================

static void build_bt_device_options(BtDeviceOptions* opts, BtDeviceInfo* info) {
	memset(opts, 0, sizeof(*opts));
	opts->dev_info = *info;
	opts->page.title = opts->dev_info.name;
	opts->page.items = opts->items;
	opts->page.is_list = 0;
	opts->page.dynamic_start = -1;

	int idx = 0;

	if (info->paired) {
		if (info->connected) {
			opts->items[idx] = (SettingItem){
				.name = "Disconnect",
				.desc = "Disconnect from this device",
				.type = ITEM_BUTTON,
				.visible = 1,
				.on_press = bt_action_disconnect,
			};
			idx++;
		} else {
			opts->items[idx] = (SettingItem){
				.name = "Connect",
				.desc = "Connect to this paired device",
				.type = ITEM_BUTTON,
				.visible = 1,
				.on_press = bt_action_connect,
			};
			idx++;
		}

		opts->items[idx] = (SettingItem){
			.name = "Unpair",
			.desc = "Remove pairing with this device",
			.type = ITEM_BUTTON,
			.visible = 1,
			.on_press = bt_action_unpair,
		};
		idx++;
	} else {
		opts->items[idx] = (SettingItem){
			.name = "Pair",
			.desc = "Pair with this device",
			.type = ITEM_BUTTON,
			.visible = 1,
			.on_press = bt_action_pair,
		};
		idx++;
	}

	opts->item_count = idx;
	opts->page.item_count = idx;
}

// ============================================
// Device list item: open options on A press
// ============================================

#define MAX_BT_OPTIONS 32
static BtDeviceOptions bt_options_pool[MAX_BT_OPTIONS];
static int bt_options_used = 0;

static void bt_device_press(void) {
	SettingsPage* page = settings_menu_current();
	if (!page)
		return;

	SettingItem* sel = settings_page_visible_item(page, page->selected);
	if (!sel || !sel->user_data)
		return;

	BtDeviceInfo* info = (BtDeviceInfo*)sel->user_data;

	if (bt_options_used >= MAX_BT_OPTIONS)
		bt_options_used = 0;
	BtDeviceOptions* opts = &bt_options_pool[bt_options_used++];
	active_bt_options = opts;

	build_bt_device_options(opts, info);
	settings_menu_push(&opts->page);
}

// ============================================
// Custom draw for device items
// ============================================

static void bt_device_draw(SDL_Surface* screen, SettingItem* item,
						   int x, int y, int w, int h, int selected) {
	BtDeviceInfo* info = (BtDeviceInfo*)item->user_data;
	if (!info)
		return;

	TTF_Font* f = font.small;
	SDL_Color text_color = UI_getListTextColor(selected);

	// Status label
	const char* status = info->connected ? "Connected"
						 : info->paired	 ? "Paired"
										 : "Available";

	// Device name (truncate first so we know the label width for pills)
	char truncated[256];
	int max_text_w = w - SCALE1(BUTTON_PADDING * 2) - SCALE1(48); // room for status + icons
	const char* display_name = info->name[0] ? info->name : info->addr;
	GFX_truncateText(f, display_name, truncated, max_text_w, 0);

	// Draw 2-layer selection pill
	if (selected) {
		// Layer 1: full-width background
		SDL_Rect row_rect = {x, y, w, h};
		GFX_blitRectColor(ASSET_BUTTON, screen, &row_rect, THEME_COLOR2);

		// Layer 2: label-width pill on top
		int text_w_px, text_h_px;
		TTF_SizeUTF8(f, truncated, &text_w_px, &text_h_px);
		int label_pill_w = text_w_px + SCALE1(BUTTON_PADDING * 2);
		SDL_Rect label_rect = {x, y, label_pill_w, h};
		GFX_blitRectColor(ASSET_BUTTON, screen, &label_rect, THEME_COLOR1);
	}

	// Device name text
	int text_x = x + SCALE1(BUTTON_PADDING);
	int text_y = y + (h - TTF_FontHeight(f)) / 2;

	SDL_Surface* text_surf = TTF_RenderUTF8_Blended(f, truncated, text_color);
	if (text_surf) {
		SDL_BlitSurface(text_surf, NULL, screen, &(SDL_Rect){text_x, text_y, 0, 0});
		SDL_FreeSurface(text_surf);
	}

	// Right side: status label + icons
	int right_x = x + w - SCALE1(BUTTON_PADDING);

	// Status label (right-aligned, like cycle values)
	SDL_Color status_color = selected ? COLOR_WHITE : UI_getListTextColor(0);
	int val_text_y = y + (h - TTF_FontHeight(font.tiny)) / 2;
	SDL_Surface* val_surf = TTF_RenderUTF8_Blended(font.tiny, status, status_color);
	if (val_surf) {
		right_x -= val_surf->w;
		SDL_BlitSurface(val_surf, NULL, screen, &(SDL_Rect){right_x, val_text_y, 0, 0});
		SDL_FreeSurface(val_surf);
		right_x -= SCALE1(4);
	}

	// Connected checkmark
	if (info->connected) {
		SDL_Rect icon_rect;
		GFX_assetRect(ASSET_CHECKCIRCLE, &icon_rect);
		right_x -= icon_rect.w;
		GFX_blitAsset(ASSET_CHECKCIRCLE, NULL, screen,
					  &(SDL_Rect){right_x, y + (h - icon_rect.h) / 2, 0, 0});
		right_x -= SCALE1(4);
	}

	// Device type icon (audio or controller)
	int type_asset = (info->device_type == BLUETOOTH_AUDIO) ? ASSET_AUDIO : ASSET_CONTROLLER;
	if (info->device_type != BLUETOOTH_NONE) {
		SDL_Rect type_rect;
		GFX_assetRect(type_asset, &type_rect);
		right_x -= type_rect.w;
		GFX_blitAsset(type_asset, NULL, screen,
					  &(SDL_Rect){right_x, y + (h - type_rect.h) / 2, 0, 0});
	}
}

// ============================================
// Scanner Thread
// ============================================

// Interruptible sleep: returns early if bt_scanner_running becomes 0
static void bt_sleep(int seconds) {
	for (int i = 0; i < seconds * 10 && bt_scanner_running; i++)
		usleep(100000); // 100ms intervals
}

static BtDeviceInfo device_info_pool[BT_MAX_ITEMS];
static int device_info_count = 0;

static void* bt_scanner(void* arg) {
	SettingsPage* page = (SettingsPage*)arg;
	int discovery_started = 0;

	while (bt_scanner_running) {
		if (!BT_enabled()) {
			// BT disabled - clear dynamic items
			pthread_rwlock_wrlock(&page->lock);
			if (page->dynamic_start >= 0)
				page->item_count = page->dynamic_start;
			device_info_count = 0;
			page->needs_layout = 1;
			pthread_rwlock_unlock(&page->lock);
			discovery_started = 0;
			bt_sleep(5);
			continue;
		}

		// Start discovery if not already running
		if (!discovery_started) {
			BT_discovery(1);
			discovery_started = 1;
		}

		// Get paired devices
		struct BT_devicePaired paired[32];
		int paired_count = BT_pairedDevices(paired, 32);

		// Get available (unpaired) devices
		struct BT_device available[32];
		int avail_count = BT_availableDevices(available, 32);

		// Save selected addr for restoration
		char selected_addr[18] = "";
		pthread_rwlock_rdlock(&page->lock);
		if (page->selected >= page->dynamic_start && page->dynamic_start >= 0) {
			SettingItem* sel = settings_page_visible_item(page, page->selected);
			if (sel && sel->user_data) {
				BtDeviceInfo* info = (BtDeviceInfo*)sel->user_data;
				strncpy(selected_addr, info->addr, sizeof(selected_addr) - 1);
			}
		}
		pthread_rwlock_unlock(&page->lock);

		// Rebuild dynamic items
		pthread_rwlock_wrlock(&page->lock);

		int dyn_start = page->dynamic_start;
		if (dyn_start < 0)
			dyn_start = BT_STATIC_COUNT;

		device_info_count = 0;
		int item_idx = dyn_start;

		// Paired devices first
		for (int i = 0; i < paired_count && item_idx < page->max_items && device_info_count < BT_MAX_ITEMS; i++) {
			BtDeviceInfo* dinfo = &device_info_pool[device_info_count++];
			strncpy(dinfo->name, paired[i].remote_name, sizeof(dinfo->name) - 1);
			strncpy(dinfo->addr, paired[i].remote_addr, sizeof(dinfo->addr) - 1);
			dinfo->paired = 1;
			dinfo->connected = paired[i].is_connected;
			dinfo->rssi = paired[i].rssi;

			dinfo->device_type = BLUETOOTH_NONE;

			page->items[item_idx] = (SettingItem){
				.name = dinfo->name[0] ? dinfo->name : dinfo->addr,
				.type = ITEM_BUTTON,
				.visible = 1,
				.on_press = bt_device_press,
				.custom_draw = bt_device_draw,
				.user_data = dinfo,
			};
			item_idx++;
		}

		// Available (unpaired) devices
		for (int i = 0; i < avail_count && item_idx < page->max_items && device_info_count < BT_MAX_ITEMS; i++) {
			// Skip if already in paired list
			int already_paired = 0;
			for (int j = 0; j < paired_count; j++) {
				if (strcmp(paired[j].remote_addr, available[i].addr) == 0) {
					already_paired = 1;
					break;
				}
			}
			if (already_paired)
				continue;

			BtDeviceInfo* dinfo = &device_info_pool[device_info_count++];
			strncpy(dinfo->name, available[i].name, sizeof(dinfo->name) - 1);
			strncpy(dinfo->addr, available[i].addr, sizeof(dinfo->addr) - 1);
			dinfo->paired = 0;
			dinfo->connected = 0;
			dinfo->rssi = 0;
			dinfo->device_type = available[i].kind;

			page->items[item_idx] = (SettingItem){
				.name = dinfo->name[0] ? dinfo->name : dinfo->addr,
				.type = ITEM_BUTTON,
				.visible = 1,
				.on_press = bt_device_press,
				.custom_draw = bt_device_draw,
				.user_data = dinfo,
			};
			item_idx++;
		}

		page->item_count = item_idx;

		// Restore selection by addr
		if (selected_addr[0]) {
			for (int i = dyn_start; i < item_idx; i++) {
				BtDeviceInfo* dinfo = (BtDeviceInfo*)page->items[i].user_data;
				if (dinfo && strcmp(dinfo->addr, selected_addr) == 0) {
					page->selected = settings_page_actual_to_visible(page, i);
					break;
				}
			}
		}

		page->needs_layout = 1;
		pthread_rwlock_unlock(&page->lock);

		bt_sleep(5);
	}

	return NULL;
}

// ============================================
// Page Lifecycle
// ============================================

static void bt_on_show(SettingsPage* page) {
	BT_init();

	// Join previous scanner if it was started
	if (bt_scanner_started) {
		pthread_join(bt_scanner_thread, NULL);
		bt_scanner_started = 0;
	}

	// Sync static items
	settings_item_sync(&page->items[BT_IDX_TOGGLE]);
	settings_item_sync(&page->items[BT_IDX_DIAG]);
	settings_item_sync(&page->items[BT_IDX_RATE]);

#ifdef HAS_BTAGENT
	btagent_start();
#endif

	// Start scanner thread
	bt_page_ref = page;
	bt_scanner_running = 1;
	bt_scanner_started = 1;
	pthread_create(&bt_scanner_thread, NULL, bt_scanner, page);
}

static void bt_on_hide(SettingsPage* page) {
	(void)page;
	bt_scanner_running = 0;
	bt_page_ref = NULL;
}

static void bt_on_tick(SettingsPage* page) {
	// Process pending layout changes from scanner thread
	if (page->needs_layout) {
		page->needs_layout = 0;
		int vis = settings_page_visible_count(page);
		if (page->selected >= vis && vis > 0)
			page->selected = vis - 1;
	}

	// Show scanning hint when BT is on but no devices listed yet
	if (BT_enabled() && page->dynamic_start >= 0 && page->item_count <= page->dynamic_start) {
		page->status_msg = "Scanning for devices...";
	} else {
		page->status_msg = NULL;
	}
}

// ============================================
// Page Creation
// ============================================

SettingsPage* bt_page_create(void) {
	SettingsPage* page = calloc(1, sizeof(SettingsPage));
	if (!page)
		return NULL;
	page->title = "Bluetooth";
	page->is_list = 0;
	page->dynamic_start = BT_STATIC_COUNT;
	page->max_items = BT_MAX_ITEMS;
	page->items = calloc(BT_MAX_ITEMS, sizeof(SettingItem));
	if (!page->items) {
		free(page);
		return NULL;
	}

	settings_page_init_lock(page);

	// BT toggle
	page->items[BT_IDX_TOGGLE] = (SettingItem){
		.name = "Bluetooth",
		.desc = "Enable or disable Bluetooth",
		.type = ITEM_CYCLE,
		.visible = 1,
		.labels = bt_toggle_labels,
		.label_count = 2,
		.get_value = bt_get_toggle,
		.set_value = bt_set_toggle,
	};

	// BT diagnostics
	page->items[BT_IDX_DIAG] = (SettingItem){
		.name = "Bluetooth diagnostics",
		.desc = "Enable Bluetooth diagnostic logging",
		.type = ITEM_CYCLE,
		.visible = 1,
		.labels = bt_diag_labels,
		.label_count = 2,
		.get_value = bt_get_diag,
		.set_value = bt_set_diag,
	};

	// Maximum sampling rate
	page->items[BT_IDX_RATE] = (SettingItem){
		.name = "Maximum sampling rate",
		.desc = "Maximum audio sampling rate for Bluetooth",
		.type = ITEM_CYCLE,
		.visible = 1,
		.labels = bt_rate_labels,
		.label_count = 2,
		.get_value = bt_get_rate,
		.set_value = bt_set_rate,
		.on_reset = bt_reset_rate,
		.values = bt_rate_values,
	};

	page->item_count = BT_STATIC_COUNT;

	page->on_show = bt_on_show;
	page->on_hide = bt_on_hide;
	page->on_tick = bt_on_tick;

	return page;
}

void bt_page_destroy(SettingsPage* page) {
	if (!page)
		return;
	bt_scanner_running = 0;
	// Don't join or wait — process exit cleans up all threads and memory
}
