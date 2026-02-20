#ifndef SETTINGS_BT_H
#define SETTINGS_BT_H

#include "settings_menu.h"

// Maximum number of BT device items (static + dynamic)
#define BT_MAX_ITEMS 64

// Create and return the Bluetooth settings page
SettingsPage* bt_page_create(void);

// Destroy the Bluetooth settings page and free resources
void bt_page_destroy(SettingsPage* page);

#endif // SETTINGS_BT_H
