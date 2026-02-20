#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "settings_wifi.h"
#include "defines.h"
#include "api.h"
#include "ui_list.h"
#include "ui_keyboard.h"
#include "ui_components.h"

// ============================================
// WiFi Network Info (attached to user_data)
// ============================================

typedef struct {
	char ssid[SSID_MAX];
	char bssid[128];
	int rssi;
	WifiSecurityType security;
	int connected;
	int known;
} WifiNetworkInfo;

// ============================================
// Static items
// ============================================

#define WIFI_STATIC_COUNT 2
#define WIFI_IDX_TOGGLE 0
#define WIFI_IDX_DIAG 1

static const char* wifi_toggle_labels[] = {"Off", "On"};
static const char* wifi_diag_labels[] = {"Off", "On"};

// ============================================
// Scanner thread state
// ============================================

static pthread_t wifi_scanner_thread;
static volatile int wifi_scanner_running = 0;
static int wifi_scanner_started = 0;
static SettingsPage* wifi_page_ref = NULL;

// ============================================
// Network options submenu (allocated per-network)
// ============================================

typedef struct {
	SettingsPage page;
	SettingItem items[4]; // max: Connect, Disconnect, Forget, + safety margin
	int item_count;
	WifiNetworkInfo net_info;
} WifiNetworkOptions;

// Forward declarations
static void wifi_on_show(SettingsPage* page);
static void wifi_on_hide(SettingsPage* page);
static void wifi_on_tick(SettingsPage* page);
static void wifi_network_draw(SDL_Surface* screen, SettingItem* item,
							  int x, int y, int w, int h, int selected);

// ============================================
// WiFi toggle (blocking with overlay)
// ============================================

static void* wifi_toggle_thread(void* arg) {
	struct {
		int val;
		volatile int* done;
	}* ctx = arg;
	WIFI_enable(ctx->val ? true : false);
	*ctx->done = 1;
	return NULL;
}

static int wifi_get_toggle(void) {
	return WIFI_enabled() ? 1 : 0;
}

static void wifi_set_toggle(int val) {
	SettingsPage* page = settings_menu_current();
	if (!page || !page->screen)
		return;

	volatile int done = 0;
	struct {
		int val;
		volatile int* done;
	} ctx = {val, &done};

	pthread_t t;
	pthread_create(&t, NULL, wifi_toggle_thread, &ctx);
	pthread_detach(t);

	const char* title = val ? "Enabling WiFi..." : "Disabling WiFi...";

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
	settings_item_sync(&page->items[WIFI_IDX_TOGGLE]);
}

static int wifi_get_diag(void) {
	return WIFI_diagnosticsEnabled() ? 1 : 0;
}

static void wifi_set_diag(int val) {
	WIFI_diagnosticsEnable(val ? true : false);
}

// ============================================
// Network option actions
// ============================================

static WifiNetworkOptions* active_net_options = NULL;

static void wifi_action_connect(void) {
	if (!active_net_options)
		return;
	WifiNetworkInfo* info = &active_net_options->net_info;

	if (info->known || info->security == SECURITY_NONE) {
		WIFI_connect(info->ssid, info->security);
	} else {
		// Need password
		char* password = UIKeyboard_open("Enter WiFi Password");
		PAD_reset(); // clear input state so A press doesn't propagate
		if (password) {
			WIFI_connectPass(info->ssid, info->security, password);
			free(password);
		}
	}

	PAD_reset(); // clear input state so A press doesn't re-trigger on main wifi page
	// Go back from options submenu
	settings_menu_pop();
	settings_menu_pop();
}

static void wifi_action_disconnect(void) {
	WIFI_disconnect();
	settings_menu_pop();
}

static void wifi_action_forget(void) {
	if (!active_net_options)
		return;
	WifiNetworkInfo* info = &active_net_options->net_info;
	WIFI_forget(info->ssid, info->security);
	settings_menu_pop();
}

// ============================================
// Build network options submenu
// ============================================

static void build_network_options(WifiNetworkOptions* opts, WifiNetworkInfo* info) {
	memset(opts, 0, sizeof(*opts));
	opts->net_info = *info;
	opts->page.title = opts->net_info.ssid;
	opts->page.items = opts->items;
	opts->page.is_list = 1;
	opts->page.dynamic_start = -1;

	int idx = 0;

	if (info->connected) {
		opts->items[idx] = (SettingItem){
			.name = "Disconnect",
			.desc = "Disconnect from this network",
			.type = ITEM_BUTTON,
			.visible = 1,
			.on_press = wifi_action_disconnect,
		};
		idx++;

		opts->items[idx] = (SettingItem){
			.name = "Forget",
			.desc = "Remove saved network credentials",
			.type = ITEM_BUTTON,
			.visible = 1,
			.on_press = wifi_action_forget,
		};
		idx++;
	} else if (info->known) {
		opts->items[idx] = (SettingItem){
			.name = "Connect",
			.desc = "Connect using saved credentials",
			.type = ITEM_BUTTON,
			.visible = 1,
			.on_press = wifi_action_connect,
		};
		idx++;

		opts->items[idx] = (SettingItem){
			.name = "Forget",
			.desc = "Remove saved network credentials",
			.type = ITEM_BUTTON,
			.visible = 1,
			.on_press = wifi_action_forget,
		};
		idx++;
	} else {
		opts->items[idx] = (SettingItem){
			.name = "Connect",
			.desc = info->security != SECURITY_NONE ? "Enter password and connect" : "Connect to open network",
			.type = ITEM_BUTTON,
			.visible = 1,
			.on_press = wifi_action_connect,
		};
		idx++;
	}

	opts->item_count = idx;
	opts->page.item_count = idx;
}

// ============================================
// Network list item: open options on A press
// ============================================

// Pool of network option submenus (reused)
#define MAX_NET_OPTIONS 32
static WifiNetworkOptions net_options_pool[MAX_NET_OPTIONS];
static int net_options_used = 0;

static void wifi_network_press(void) {
	// This gets called when A is pressed on a network item.
	// We need to find which network was selected from the current page.
	SettingsPage* page = settings_menu_current();
	if (!page)
		return;

	SettingItem* sel = settings_page_visible_item(page, page->selected);
	if (!sel || !sel->user_data)
		return;

	WifiNetworkInfo* info = (WifiNetworkInfo*)sel->user_data;

	// Reuse options pool
	if (net_options_used >= MAX_NET_OPTIONS)
		net_options_used = 0;
	WifiNetworkOptions* opts = &net_options_pool[net_options_used++];
	active_net_options = opts;

	build_network_options(opts, info);
	settings_menu_push(&opts->page);
}

// ============================================
// Custom draw for network items
// ============================================

static void wifi_network_draw(SDL_Surface* screen, SettingItem* item,
							  int x, int y, int w, int h, int selected) {
	WifiNetworkInfo* info = (WifiNetworkInfo*)item->user_data;
	if (!info)
		return;

	TTF_Font* f = font.small;
	SDL_Color text_color = UI_getListTextColor(selected);

	// SSID text (truncate first so we know the label width for pills)
	char truncated[128];
	int max_text_w = w - SCALE1(BUTTON_PADDING * 2) - SCALE1(48); // room for icons
	GFX_truncateText(f, info->ssid, truncated, max_text_w, 0);

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

	int text_x = x + SCALE1(BUTTON_PADDING);
	int text_y = y + (h - TTF_FontHeight(f)) / 2;

	SDL_Surface* text_surf = TTF_RenderUTF8_Blended(f, truncated, text_color);
	if (text_surf) {
		SDL_BlitSurface(text_surf, NULL, screen, &(SDL_Rect){text_x, text_y, 0, 0});
		SDL_FreeSurface(text_surf);
	}

	// Right-side icons
	int icon_x = x + w - SCALE1(BUTTON_PADDING);

	// Connected checkmark or lock icon
	if (info->connected) {
		SDL_Rect icon_rect;
		GFX_assetRect(ASSET_CHECKCIRCLE, &icon_rect);
		icon_x -= icon_rect.w;
		GFX_blitAsset(ASSET_CHECKCIRCLE, NULL, screen,
					  &(SDL_Rect){icon_x, y + (h - icon_rect.h) / 2, 0, 0});
		icon_x -= SCALE1(4);
	} else if (info->security != SECURITY_NONE) {
		SDL_Rect icon_rect;
		GFX_assetRect(ASSET_LOCK, &icon_rect);
		icon_x -= icon_rect.w;
		GFX_blitAsset(ASSET_LOCK, NULL, screen,
					  &(SDL_Rect){icon_x, y + (h - icon_rect.h) / 2, 0, 0});
		icon_x -= SCALE1(4);
	}

	// Signal strength icon
	int signal_asset;
	if (info->rssi > -50)
		signal_asset = ASSET_WIFI;
	else if (info->rssi > -70)
		signal_asset = ASSET_WIFI_MED;
	else
		signal_asset = ASSET_WIFI_LOW;

	SDL_Rect sig_rect;
	GFX_assetRect(signal_asset, &sig_rect);
	icon_x -= sig_rect.w;
	GFX_blitAsset(signal_asset, NULL, screen,
				  &(SDL_Rect){icon_x, y + (h - sig_rect.h) / 2, 0, 0});
}

// ============================================
// Scanner Thread
// ============================================

// Interruptible sleep: returns early if wifi_scanner_running becomes 0
static void wifi_sleep(int seconds) {
	for (int i = 0; i < seconds * 10 && wifi_scanner_running; i++)
		usleep(100000); // 100ms intervals
}

// Pool of WifiNetworkInfo for dynamic items
static WifiNetworkInfo network_info_pool[SCAN_MAX_RESULTS];
static int network_info_count = 0;

static void* wifi_scanner(void* arg) {
	SettingsPage* page = (SettingsPage*)arg;

	while (wifi_scanner_running) {
		if (!WIFI_enabled()) {
			// WiFi disabled - clear dynamic items
			pthread_rwlock_wrlock(&page->lock);
			if (page->dynamic_start >= 0)
				page->item_count = page->dynamic_start;
			network_info_count = 0;
			page->needs_layout = 1;
			pthread_rwlock_unlock(&page->lock);
			wifi_sleep(5);
			continue;
		}

		// Get connection info
		struct WIFI_connection conn;
		int has_conn = (WIFI_connectionInfo(&conn) == 0 && conn.valid);

		// Scan networks
		struct WIFI_network networks[SCAN_MAX_RESULTS];
		int count = WIFI_scan(networks, SCAN_MAX_RESULTS);

		if (count > 0) {
			// Deduplicate by SSID (keep strongest signal)
			struct WIFI_network deduped[SCAN_MAX_RESULTS];
			int dedup_count = 0;

			for (int i = 0; i < count; i++) {
				if (networks[i].ssid[0] == '\0')
					continue;

				int found = 0;
				for (int j = 0; j < dedup_count; j++) {
					if (strcmp(deduped[j].ssid, networks[i].ssid) == 0) {
						if (networks[i].rssi > deduped[j].rssi)
							deduped[j] = networks[i];
						found = 1;
						break;
					}
				}
				if (!found && dedup_count < SCAN_MAX_RESULTS) {
					deduped[dedup_count++] = networks[i];
				}
			}

			// Save selected SSID for restoration
			char selected_ssid[SSID_MAX] = "";
			pthread_rwlock_rdlock(&page->lock);
			if (page->selected >= page->dynamic_start && page->dynamic_start >= 0) {
				SettingItem* sel = settings_page_visible_item(page, page->selected);
				if (sel && sel->user_data) {
					WifiNetworkInfo* info = (WifiNetworkInfo*)sel->user_data;
					strncpy(selected_ssid, info->ssid, sizeof(selected_ssid) - 1);
				}
			}
			pthread_rwlock_unlock(&page->lock);

			// Rebuild dynamic items
			pthread_rwlock_wrlock(&page->lock);

			int dyn_start = page->dynamic_start;
			if (dyn_start < 0)
				dyn_start = WIFI_STATIC_COUNT;

			network_info_count = 0;
			int item_idx = dyn_start;

			for (int i = 0; i < dedup_count && item_idx < page->max_items; i++) {
				WifiNetworkInfo* ninfo = &network_info_pool[network_info_count++];
				strncpy(ninfo->ssid, deduped[i].ssid, sizeof(ninfo->ssid) - 1);
				strncpy(ninfo->bssid, deduped[i].bssid, sizeof(ninfo->bssid) - 1);
				ninfo->rssi = deduped[i].rssi;
				ninfo->security = deduped[i].security;
				ninfo->connected = (has_conn && strcmp(conn.ssid, deduped[i].ssid) == 0);
				ninfo->known = WIFI_isKnown(ninfo->ssid, ninfo->security);

				page->items[item_idx] = (SettingItem){
					.name = ninfo->ssid,
					.desc = ninfo->connected ? "Connected" : "",
					.type = ITEM_BUTTON,
					.visible = 1,
					.on_press = wifi_network_press,
					.custom_draw = wifi_network_draw,
					.user_data = ninfo,
				};
				item_idx++;
			}

			page->item_count = item_idx;

			// Restore selection by SSID
			if (selected_ssid[0]) {
				for (int i = dyn_start; i < item_idx; i++) {
					WifiNetworkInfo* ninfo = (WifiNetworkInfo*)page->items[i].user_data;
					if (ninfo && strcmp(ninfo->ssid, selected_ssid) == 0) {
						page->selected = settings_page_actual_to_visible(page, i);
						break;
					}
				}
			}

			page->needs_layout = 1;
			pthread_rwlock_unlock(&page->lock);
		} else {
			// Clear stale dynamic items when scan returns empty
			pthread_rwlock_wrlock(&page->lock);
			if (page->dynamic_start >= 0)
				page->item_count = page->dynamic_start;
			network_info_count = 0;
			page->needs_layout = 1;
			pthread_rwlock_unlock(&page->lock);
		}

		wifi_sleep(3);
	}

	return NULL;
}

// ============================================
// Page Lifecycle
// ============================================

static void wifi_on_show(SettingsPage* page) {
	WIFI_init();

	// Join previous scanner if it was started
	if (wifi_scanner_started) {
		pthread_join(wifi_scanner_thread, NULL);
		wifi_scanner_started = 0;
	}

	// Sync static items
	settings_item_sync(&page->items[WIFI_IDX_TOGGLE]);
	settings_item_sync(&page->items[WIFI_IDX_DIAG]);

	// Start scanner thread
	wifi_page_ref = page;
	wifi_scanner_running = 1;
	wifi_scanner_started = 1;
	pthread_create(&wifi_scanner_thread, NULL, wifi_scanner, page);
}

static void wifi_on_hide(SettingsPage* page) {
	(void)page;
	wifi_scanner_running = 0;
	wifi_page_ref = NULL;
}

static void wifi_on_tick(SettingsPage* page) {
	// Process pending layout changes from scanner thread
	if (page->needs_layout) {
		page->needs_layout = 0;
		int vis = settings_page_visible_count(page);
		if (page->selected >= vis && vis > 0)
			page->selected = vis - 1;
	}

	// Show scanning hint when WiFi is on but no networks listed yet
	if (WIFI_enabled() && page->dynamic_start >= 0 && page->item_count <= page->dynamic_start) {
		page->status_msg = "Scanning for networks...";
	} else {
		page->status_msg = NULL;
	}
}

// ============================================
// Page Creation
// ============================================

SettingsPage* wifi_page_create(void) {
	SettingsPage* page = calloc(1, sizeof(SettingsPage));
	if (!page)
		return NULL;
	page->title = "Network";
	page->is_list = 0;
	page->dynamic_start = WIFI_STATIC_COUNT;
	page->max_items = WIFI_MAX_ITEMS;
	page->items = calloc(WIFI_MAX_ITEMS, sizeof(SettingItem));
	if (!page->items) {
		free(page);
		return NULL;
	}

	settings_page_init_lock(page);

	// WiFi toggle
	page->items[WIFI_IDX_TOGGLE] = (SettingItem){
		.name = "WiFi",
		.desc = "Enable or disable WiFi",
		.type = ITEM_CYCLE,
		.visible = 1,
		.labels = wifi_toggle_labels,
		.label_count = 2,
		.get_value = wifi_get_toggle,
		.set_value = wifi_set_toggle,
	};

	// WiFi diagnostics
	page->items[WIFI_IDX_DIAG] = (SettingItem){
		.name = "WiFi diagnostics",
		.desc = "Enable WiFi diagnostic logging",
		.type = ITEM_CYCLE,
		.visible = 1,
		.labels = wifi_diag_labels,
		.label_count = 2,
		.get_value = wifi_get_diag,
		.set_value = wifi_set_diag,
	};

	page->item_count = WIFI_STATIC_COUNT;

	page->on_show = wifi_on_show;
	page->on_hide = wifi_on_hide;
	page->on_tick = wifi_on_tick;

	return page;
}

void wifi_page_destroy(SettingsPage* page) {
	if (!page)
		return;
	wifi_scanner_running = 0;
	// Don't join or wait — process exit cleans up all threads and memory
}
