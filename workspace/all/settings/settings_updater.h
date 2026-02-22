#ifndef SETTINGS_UPDATER_H
#define SETTINGS_UPDATER_H

#include "settings_menu.h"

// About page on_show callback - starts background update check
void updater_about_on_show(SettingsPage* page);

// About page on_tick callback - processes background check results
void updater_about_on_tick(SettingsPage* page);

// Button callback - installs update if available, retries check on error
void updater_check_for_updates(void);

#endif // SETTINGS_UPDATER_H
