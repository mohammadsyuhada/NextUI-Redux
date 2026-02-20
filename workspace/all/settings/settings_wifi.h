#ifndef SETTINGS_WIFI_H
#define SETTINGS_WIFI_H

#include "settings_menu.h"

// Maximum number of WiFi network items (static + dynamic)
#define WIFI_MAX_ITEMS 64

// Create and return the WiFi settings page
SettingsPage* wifi_page_create(void);

// Destroy the WiFi settings page and free resources
void wifi_page_destroy(SettingsPage* page);

#endif // SETTINGS_WIFI_H
