#ifndef SETTINGS_DEVELOPER_H
#define SETTINGS_DEVELOPER_H

#include "settings_menu.h"

// Create and return the Developer settings page
SettingsPage* developer_page_create(DevicePlatform dev_platform);

// Destroy the Developer settings page
void developer_page_destroy(SettingsPage* page);

#endif // SETTINGS_DEVELOPER_H
