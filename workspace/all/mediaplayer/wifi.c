#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "api.h"
#include "wifi.h"

// WiFi connection timeout (in 500ms intervals)
#define WIFI_CONNECT_TIMEOUT_INTERVALS 10 // 5 seconds total

// Render a simple "Connecting..." screen
static void render_connecting_screen(SDL_Surface* scr, IndicatorType show_setting) {
	// Clear GPU scroll text layer to prevent bleeding through
	GFX_clearLayers(LAYER_SCROLLTEXT);
	GFX_clear(scr);

	int hw = scr->w;
	int hh = scr->h;

	// Center the message
	const char* msg = "Connecting to WiFi...";
	SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
	if (text) {
		SDL_BlitSurface(text, NULL, scr, &(SDL_Rect){(hw - text->w) / 2, (hh - text->h) / 2});
		SDL_FreeSurface(text);
	}

	GFX_blitHardwareGroup(scr, show_setting);
	GFX_flip(scr);
}

// Check if WiFi is currently connected
bool Wifi_isConnected(void) {
	return PLAT_wifiEnabled() && PLAT_wifiConnected();
}

// Ensure WiFi is connected, enabling if necessary
// Returns true if connected, false otherwise
// Shows "Connecting..." screen while waiting (if scr is not NULL)
// Can be called from background threads with scr=NULL to skip UI rendering
bool Wifi_ensureConnected(SDL_Surface* scr, IndicatorType show_setting) {
	// Already connected?
	if (Wifi_isConnected()) {
		return true;
	}

	// Only render if screen is provided (not from background thread)
	if (scr) {
		render_connecting_screen(scr, show_setting);
	}

	// If WiFi is disabled, enable it
	if (!PLAT_wifiEnabled()) {
		PLAT_wifiEnable(true);
		// Give wpa_supplicant time to start
		usleep(1000000); // 1 second
	}

	// Enable all saved networks (they may be disabled by select_network)
	system("wpa_cli -p /etc/wifi/sockets -i wlan0 enable_network all > /dev/null 2>&1");

	// Trigger reconnect to saved networks
	system("wpa_cli -p /etc/wifi/sockets -i wlan0 reconnect > /dev/null 2>&1");

	// Wait for connection to a known network
	for (int i = 0; i < WIFI_CONNECT_TIMEOUT_INTERVALS; i++) {
		if (PLAT_wifiConnected()) {
			// Request IP via DHCP
			system("killall udhcpc 2>/dev/null; udhcpc -i wlan0 -b 2>/dev/null &");
			// Wait briefly for DHCP to complete
			usleep(1500000); // 1.5 seconds
			return true;
		}
		usleep(500000); // 500ms

		// Keep rendering and processing events while waiting (only if screen provided)
		if (scr) {
			PAD_poll();
			render_connecting_screen(scr, show_setting);
		}
	}

	// Final check
	if (PLAT_wifiConnected()) {
		// Request IP via DHCP
		system("killall udhcpc 2>/dev/null; udhcpc -i wlan0 -b 2>/dev/null &");
		usleep(1500000); // 1.5 seconds
		return true;
	}
	return false;
}
