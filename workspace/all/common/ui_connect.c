#include "ui_connect.h"
#include "ui_listdialog.h"
#include "ui_keyboard.h"
#include "api.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
	CS_INIT,
	CS_SCANNING,
	CS_LIST,
	CS_CONNECTING,
	CS_DONE,
} ConnectState;

static ConnectState state;
static bool is_wifi;
static uint32_t last_scan_time;
static uint32_t connect_start_time;

#define WIFI_SCAN_INTERVAL_MS 10000 // WiFi scan blocks ~2s, don't rescan too often
#define BT_SCAN_INTERVAL_MS 5000	// BT discovery is async, poll periodically
#define CONNECT_TIMEOUT_MS 10000

// WiFi scan results cache
static struct WIFI_network wifi_networks[SCAN_MAX_RESULTS];
static int wifi_network_count;
static struct WIFI_connection wifi_conn;

// Bluetooth scan results cache
static struct BT_device bt_available[SCAN_MAX_RESULTS];
static int bt_available_count;
static struct BT_devicePaired bt_paired[SCAN_MAX_RESULTS];
static int bt_paired_count;

// ---- WiFi helpers ----

static int wifi_signal_asset(int rssi) {
	if (rssi >= -50)
		return ASSET_WIFI;
	if (rssi >= -70)
		return ASSET_WIFI_MED;
	return ASSET_WIFI_LOW;
}

static void wifi_populate_list(void) {
	ListDialogItem items[SCAN_MAX_RESULTS];
	int count = 0;

	// Get current connection info
	bool connected = WIFI_connected();
	if (connected)
		WIFI_connectionInfo(&wifi_conn);

	for (int i = 0; i < wifi_network_count && count < SCAN_MAX_RESULTS; i++) {
		struct WIFI_network* net = &wifi_networks[i];
		if (net->ssid[0] == '\0')
			continue;

		// Skip duplicates (same SSID, keep strongest signal)
		bool duplicate = false;
		for (int j = 0; j < count; j++) {
			if (strcmp(items[j].text, net->ssid) == 0) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;

		ListDialogItem* item = &items[count];
		strncpy(item->text, net->ssid, LISTDIALOG_MAX_TEXT - 1);
		item->text[LISTDIALOG_MAX_TEXT - 1] = '\0';

		item->detail[0] = '\0';
		item->prepend_icons[0] = -1;
		if (connected && strcmp(wifi_conn.ssid, net->ssid) == 0) {
			item->append_icons[0] = ASSET_CHECKCIRCLE;
			item->append_icons[1] = -1;
		} else {
			int ai = 0;
			if (net->security != SECURITY_NONE)
				item->append_icons[ai++] = ASSET_LOCK;
			item->append_icons[ai++] = wifi_signal_asset(net->rssi);
			item->append_icons[ai] = -1;
		}

		count++;
	}

	ListDialog_setItems(items, count);
	if (count > 0)
		ListDialog_setStatus(NULL);
}

static void bt_populate_list(void) {
	ListDialogItem items[SCAN_MAX_RESULTS];
	int count = 0;

	// Paired devices first
	bt_paired_count = BT_pairedDevices(bt_paired, SCAN_MAX_RESULTS);
	for (int i = 0; i < bt_paired_count && count < SCAN_MAX_RESULTS; i++) {
		struct BT_devicePaired* dev = &bt_paired[i];
		ListDialogItem* item = &items[count];

		strncpy(item->text, dev->remote_name[0] ? dev->remote_name : dev->remote_addr,
				LISTDIALOG_MAX_TEXT - 1);
		item->text[LISTDIALOG_MAX_TEXT - 1] = '\0';

		item->prepend_icons[0] = -1;
		if (dev->is_connected) {
			item->detail[0] = '\0';
			item->append_icons[0] = ASSET_CHECKCIRCLE;
			item->append_icons[1] = -1;
		} else {
			strncpy(item->detail, "Paired", LISTDIALOG_MAX_TEXT - 1);
			item->detail[LISTDIALOG_MAX_TEXT - 1] = '\0';
			item->append_icons[0] = -1;
		}
		count++;
	}

	// Available (not yet paired) devices
	bt_available_count = BT_availableDevices(bt_available, SCAN_MAX_RESULTS);
	for (int i = 0; i < bt_available_count && count < SCAN_MAX_RESULTS; i++) {
		struct BT_device* dev = &bt_available[i];

		// Skip if already in paired list
		bool already_listed = false;
		for (int j = 0; j < bt_paired_count; j++) {
			if (strcmp(bt_paired[j].remote_addr, dev->addr) == 0) {
				already_listed = true;
				break;
			}
		}
		if (already_listed)
			continue;

		ListDialogItem* item = &items[count];
		strncpy(item->text, dev->name[0] ? dev->name : dev->addr,
				LISTDIALOG_MAX_TEXT - 1);
		item->text[LISTDIALOG_MAX_TEXT - 1] = '\0';

		item->detail[0] = '\0';
		item->prepend_icons[0] = -1;

		if (dev->kind == BLUETOOTH_AUDIO)
			item->append_icons[0] = ASSET_AUDIO;
		else if (dev->kind == BLUETOOTH_CONTROLLER)
			item->append_icons[0] = ASSET_CONTROLLER;
		else
			item->append_icons[0] = ASSET_BLUETOOTH;
		item->append_icons[1] = -1;

		count++;
	}

	ListDialog_setItems(items, count);
	if (count > 0)
		ListDialog_setStatus(NULL);
}

// ---- Find network/device by list index ----

// Returns the WIFI_network matching the given list index, accounting for
// duplicate SSID filtering done in wifi_populate_list
static struct WIFI_network* wifi_find_by_index(int index) {
	int seen = 0;
	char seen_ssids[SCAN_MAX_RESULTS][SSID_MAX];
	int seen_count = 0;

	for (int i = 0; i < wifi_network_count; i++) {
		struct WIFI_network* net = &wifi_networks[i];
		if (net->ssid[0] == '\0')
			continue;

		bool duplicate = false;
		for (int j = 0; j < seen_count; j++) {
			if (strcmp(seen_ssids[j], net->ssid) == 0) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;

		strncpy(seen_ssids[seen_count], net->ssid, SSID_MAX - 1);
		seen_ssids[seen_count][SSID_MAX - 1] = '\0';
		seen_count++;

		if (seen == index)
			return net;
		seen++;
	}
	return NULL;
}

// Returns the address of the BT device at the given list index.
// Paired devices come first, then available devices (matching bt_populate_list order).
static const char* bt_find_addr_by_index(int index) {
	// Paired devices first
	if (index < bt_paired_count)
		return bt_paired[index].remote_addr;

	// Then available (non-paired) devices
	int avail_idx = index - bt_paired_count;
	int skip_count = 0;
	for (int i = 0; i < bt_available_count; i++) {
		bool already_paired = false;
		for (int j = 0; j < bt_paired_count; j++) {
			if (strcmp(bt_paired[j].remote_addr, bt_available[i].addr) == 0) {
				already_paired = true;
				break;
			}
		}
		if (already_paired)
			continue;

		if (skip_count == avail_idx)
			return bt_available[i].addr;
		skip_count++;
	}
	return NULL;
}

static bool bt_is_paired_index(int index) {
	return index < bt_paired_count;
}

static bool bt_is_connected_index(int index) {
	if (index < bt_paired_count)
		return bt_paired[index].is_connected;
	return false;
}

// ---- Public API ----

void ConnectDialog_initWifi(void) {
	is_wifi = true;
	state = CS_INIT;
	last_scan_time = 0;
	connect_start_time = 0;
	wifi_network_count = 0;
	memset(&wifi_conn, 0, sizeof(wifi_conn));
}

void ConnectDialog_initBluetooth(void) {
	is_wifi = false;
	state = CS_INIT;
	last_scan_time = 0;
	connect_start_time = 0;
	bt_available_count = 0;
	bt_paired_count = 0;
}

ConnectResult ConnectDialog_handleInput(void) {
	ConnectResult result = {CONNECT_NONE, true};
	uint32_t now = SDL_GetTicks();

	switch (state) {
	case CS_INIT:
		if (is_wifi) {
			if (!WIFI_enabled())
				WIFI_enable(true);
			ListDialog_init("WiFi Networks");
			ListDialog_setStatus("Scanning...");
		} else {
			if (!BT_enabled())
				BT_enable(true);
			ListDialog_init("Bluetooth Devices");
			ListDialog_setStatus("Scanning...");
			BT_discovery(1);
		}
		state = CS_SCANNING;
		last_scan_time = now;
		break;

	case CS_SCANNING:
		if (is_wifi) {
			wifi_network_count = WIFI_scan(wifi_networks, SCAN_MAX_RESULTS);
			wifi_populate_list();
			state = CS_LIST;
		} else {
			// BT discovery is async, poll for results
			bt_populate_list();
			state = CS_LIST;
		}
		last_scan_time = now;
		break;

	case CS_LIST: {
		// Handle input first so navigation is never blocked by rescan
		ListDialogResult lr = ListDialog_handleInput();

		// Periodic rescan (only when no input was acted on)
		if (lr.action == LISTDIALOG_NONE) {
			uint32_t scan_interval = is_wifi ? WIFI_SCAN_INTERVAL_MS : BT_SCAN_INTERVAL_MS;
			if (now - last_scan_time > scan_interval) {
				if (is_wifi) {
					wifi_network_count = WIFI_scan(wifi_networks, SCAN_MAX_RESULTS);
					wifi_populate_list();
				} else {
					bt_populate_list();
				}
				last_scan_time = now;
			}
		}

		if (lr.action == LISTDIALOG_CANCEL) {
			if (!is_wifi)
				BT_discovery(0);
			result.action = CONNECT_CANCEL;
			return result;
		}

		if (lr.action == LISTDIALOG_SELECTED) {
			if (is_wifi) {
				struct WIFI_network* net = wifi_find_by_index(lr.index);
				if (!net)
					break;

				// If already connected to this network, disconnect
				if (WIFI_connected()) {
					WIFI_connectionInfo(&wifi_conn);
					if (strcmp(wifi_conn.ssid, net->ssid) == 0) {
						WIFI_disconnect();
						// Rescan after disconnect
						wifi_network_count = WIFI_scan(wifi_networks, SCAN_MAX_RESULTS);
						wifi_populate_list();
						break;
					}
				}

				// If we have stored credentials, connect directly
				if (WIFI_isKnown(net->ssid, net->security)) {
					WIFI_connect(net->ssid, net->security);
					ListDialog_setStatus("Connecting...");
					ListDialog_setItems(NULL, 0);
					state = CS_CONNECTING;
					connect_start_time = now;
				} else if (net->security != SECURITY_NONE) {
					// Need password
					char* password = UIKeyboard_open("Password");
					PAD_reset(); // clear input state after keyboard binary exits
					if (password) {
						WIFI_connectPass(net->ssid, net->security, password);
						free(password);
						ListDialog_setStatus("Connecting...");
						ListDialog_setItems(NULL, 0);
						state = CS_CONNECTING;
						connect_start_time = now;
					}
					// If cancelled, stay on list
				} else {
					// Open network
					WIFI_connect(net->ssid, net->security);
					ListDialog_setStatus("Connecting...");
					ListDialog_setItems(NULL, 0);
					state = CS_CONNECTING;
					connect_start_time = now;
				}
			} else {
				// Bluetooth
				const char* addr = bt_find_addr_by_index(lr.index);
				if (!addr)
					break;

				// Need a mutable copy for BT APIs
				char addr_buf[18];
				strncpy(addr_buf, addr, sizeof(addr_buf) - 1);
				addr_buf[sizeof(addr_buf) - 1] = '\0';

				if (bt_is_connected_index(lr.index)) {
					BT_disconnect(addr_buf);
					bt_populate_list();
				} else if (bt_is_paired_index(lr.index)) {
					BT_connect(addr_buf);
					ListDialog_setStatus("Connecting...");
					ListDialog_setItems(NULL, 0);
					state = CS_CONNECTING;
					connect_start_time = now;
				} else {
					// Not paired - pair first then connect
					BT_pair(addr_buf);
					BT_connect(addr_buf);
					ListDialog_setStatus("Pairing...");
					ListDialog_setItems(NULL, 0);
					state = CS_CONNECTING;
					connect_start_time = now;
				}
			}
		}
		break;
	}

	case CS_CONNECTING:
		if (is_wifi) {
			if (WIFI_connected()) {
				result.action = CONNECT_DONE;
				return result;
			}
		} else {
			if (BT_isConnected()) {
				BT_discovery(0);
				result.action = CONNECT_DONE;
				return result;
			}
		}

		// Timeout - go back to list
		if (now - connect_start_time > CONNECT_TIMEOUT_MS) {
			ListDialog_setStatus("Connection failed");
			state = CS_LIST;
			last_scan_time = 0; // trigger immediate rescan
		}
		break;

	case CS_DONE:
		result.action = CONNECT_DONE;
		return result;
	}

	return result;
}

void ConnectDialog_render(SDL_Surface* screen) {
	ListDialog_render(screen);
}

void ConnectDialog_quit(void) {
	ListDialog_quit();
	state = CS_DONE;
}
