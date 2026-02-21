/*
 * settings_developer.c - Developer settings for NextUI Settings
 *
 * Provides developer-oriented options: disable sleep, SSH toggle,
 * and SSH on boot within the settings framework.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "settings_developer.h"
#include "defines.h"
#include "api.h"
#include "ui_components.h"

// ============================================
// Developer settings page
// ============================================

#define DEV_ITEM_COUNT 4
#define DEV_IDX_DISABLE_SLEEP 0
#define DEV_IDX_SSH_TOGGLE 1
#define DEV_IDX_SSH_ON_BOOT 2
#define DEV_IDX_CLEAN_DOTFILES 3

static const char* on_off_labels[] = {"Off", "On"};
static int on_off_values[] = {0, 1};

// Platform stored for SSH password display
static DevicePlatform current_platform = PLAT_UNKNOWN;

// Track SSH runtime state (not persisted)
static int ssh_running = 0;

// ============================================
// Disable sleep
// ============================================

static int dev_get_disable_sleep(void) {
	return CFG_getDisableSleep() ? 1 : 0;
}

static void dev_set_disable_sleep(int v) {
	CFG_setDisableSleep(v != 0);
}

static void dev_reset_disable_sleep(void) {
	CFG_setDisableSleep(CFG_DEFAULT_DISABLE_SLEEP);
}

// ============================================
// Enable SSH (runtime toggle with overlay)
// ============================================

static int dev_ssh_check_running(void) {
	// Check if sshd process is running
	int ret = system("pidof sshd > /dev/null 2>&1");
	ssh_running = (ret == 0) ? 1 : 0;
	return ssh_running;
}

static void* ssh_toggle_thread(void* arg) {
	struct {
		int val;
		volatile int* done;
	}* ctx = arg;

	if (ctx->val) {
		system("/etc/init.d/sshd start > /dev/null 2>&1");
	} else {
		system("/etc/init.d/sshd stop > /dev/null 2>&1");
	}
	*ctx->done = 1;
	return NULL;
}

static int dev_get_ssh(void) {
	return dev_ssh_check_running();
}

static void dev_set_ssh(int val) {
	SettingsPage* page = settings_menu_current();
	if (!page || !page->screen)
		return;

	volatile int done = 0;
	struct {
		int val;
		volatile int* done;
	} ctx = {val, &done};

	pthread_t t;
	pthread_create(&t, NULL, ssh_toggle_thread, &ctx);
	pthread_detach(t);

	const char* title = val ? "Starting SSH..." : "Stopping SSH...";

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

	// Update runtime state
	dev_ssh_check_running();

	// Re-sync the SSH toggle item with actual state
	settings_item_sync(&page->items[DEV_IDX_SSH_TOGGLE]);
}

static void dev_reset_ssh(void) {
	// Reset = stop SSH
	if (ssh_running) {
		dev_set_ssh(0);
	}
}

// ============================================
// SSH on boot
// ============================================

static int dev_get_ssh_on_boot(void) {
	return CFG_getSSHOnBoot() ? 1 : 0;
}

static void dev_set_ssh_on_boot(int v) {
	CFG_setSSHOnBoot(v != 0);
}

static void dev_reset_ssh_on_boot(void) {
	CFG_setSSHOnBoot(CFG_DEFAULT_SSH_ON_BOOT);
}

// ============================================
// Clean dot files
// ============================================

static volatile int dotclean_done = 0;
static int dotclean_count = 0;

static void* dotclean_thread(void* arg) {
	(void)arg;
	dotclean_count = 0;

	// Build and run a shell command that finds and deletes macOS dot files
	// Matches: .Spotlight-V100, .apDisk, .fseventsd, .TemporaryItems,
	//          .Trash, .Trashes, ._*, .DS_Store, *_cache[0-9].db, __MACOSX
	const char* cmd =
		"cd " SDCARD_PATH " && "
		"{"
		" find . -maxdepth 1 \\( -name '.Spotlight-V100' -o -name '.apDisk'"
		" -o -name '.fseventsd' -o -name '.TemporaryItems'"
		" -o -name '.Trash' -o -name '.Trashes' \\);"
		" find . -depth -type f \\( -name '._*' -o -name '.DS_Store'"
		" -o -name '*_cache[0-9].db' \\);"
		" find . -depth -type d -name '__MACOSX';"
		"} 2>/dev/null";

	FILE* fp = popen(cmd, "r");
	if (fp) {
		char line[1024];
		while (fgets(line, sizeof(line), fp)) {
			// Strip trailing newline
			size_t len = strlen(line);
			if (len > 0 && line[len - 1] == '\n')
				line[len - 1] = '\0';
			if (line[0] == '\0')
				continue;

			// Build full path and remove
			char fullpath[1280];
			snprintf(fullpath, sizeof(fullpath), SDCARD_PATH "/%s", line + 2); // skip "./"
			// Use rm -rf for both files and directories
			char rm_cmd[1400];
			snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", fullpath);
			system(rm_cmd);
			dotclean_count++;
		}
		pclose(fp);
	}

	dotclean_done = 1;
	return NULL;
}

static void dev_clean_dotfiles(void) {
	SettingsPage* page = settings_menu_current();
	if (!page || !page->screen)
		return;

	dotclean_done = 0;
	dotclean_count = 0;

	pthread_t t;
	pthread_create(&t, NULL, dotclean_thread, NULL);
	pthread_detach(t);

	while (!dotclean_done) {
		GFX_startFrame();
		PAD_poll();

		GFX_clear(page->screen);
		settings_menu_render(page->screen, 0);
		UI_renderLoadingOverlay(page->screen, "Cleaning dot files...", NULL);
		GFX_flip(page->screen);
	}

	// Show result
	char msg[128];
	if (dotclean_count == 0) {
		snprintf(msg, sizeof(msg), "Nothing to clean up.");
	} else {
		snprintf(msg, sizeof(msg), "Deleted %d item%s.",
				 dotclean_count, dotclean_count == 1 ? "" : "s");
	}

	// Show result for ~2 seconds
	unsigned long start = SDL_GetTicks();
	while (SDL_GetTicks() - start < 2000) {
		GFX_startFrame();
		PAD_poll();
		if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B))
			break;

		GFX_clear(page->screen);
		settings_menu_render(page->screen, 0);
		UI_renderLoadingOverlay(page->screen, msg, NULL);
		GFX_flip(page->screen);
	}
}

// ============================================
// Dynamic description for SSH item
// ============================================

static char ssh_desc_buf[128];

static const char* dev_get_ssh_desc(void) {
	if (ssh_running) {
		if (current_platform == PLAT_TG5050) {
			snprintf(ssh_desc_buf, sizeof(ssh_desc_buf),
					 "SSH active. No password required.");
		} else {
			snprintf(ssh_desc_buf, sizeof(ssh_desc_buf),
					 "SSH active. Password: tina");
		}
	} else {
		snprintf(ssh_desc_buf, sizeof(ssh_desc_buf),
				 "Start SSH server for remote access.");
	}
	return ssh_desc_buf;
}

// ============================================
// Page lifecycle
// ============================================

static void dev_on_show(SettingsPage* page) {
	// Re-check SSH status when page is shown
	dev_ssh_check_running();
	// Update SSH item description
	if (page->item_count > DEV_IDX_SSH_TOGGLE) {
		page->items[DEV_IDX_SSH_TOGGLE].desc = dev_get_ssh_desc();
	}
	// Sync all items
	for (int i = 0; i < page->item_count; i++) {
		settings_item_sync(&page->items[i]);
	}
}

static void dev_on_tick(SettingsPage* page) {
	// Update SSH description dynamically
	if (page->item_count > DEV_IDX_SSH_TOGGLE) {
		page->items[DEV_IDX_SSH_TOGGLE].desc = dev_get_ssh_desc();
	}
}

// ============================================
// Page create / destroy
// ============================================

SettingsPage* developer_page_create(DevicePlatform dev_platform) {
	current_platform = dev_platform;

	// Check initial SSH state
	dev_ssh_check_running();

	SettingsPage* page = calloc(1, sizeof(SettingsPage));
	if (!page)
		return NULL;

	SettingItem* items = calloc(DEV_ITEM_COUNT, sizeof(SettingItem));
	if (!items) {
		free(page);
		return NULL;
	}

	int idx = 0;

	items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Disable sleep", "Prevent deep sleep mode. Useful for ADB debugging.",
		on_off_labels, 2, on_off_values,
		dev_get_disable_sleep, dev_set_disable_sleep, dev_reset_disable_sleep);

	items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Enable SSH", dev_get_ssh_desc(),
		on_off_labels, 2, on_off_values,
		dev_get_ssh, dev_set_ssh, dev_reset_ssh);

	items[idx++] = (SettingItem)ITEM_CYCLE_INIT(
		"Start SSH on boot", "Automatically start SSH when device boots.",
		on_off_labels, 2, on_off_values,
		dev_get_ssh_on_boot, dev_set_ssh_on_boot, dev_reset_ssh_on_boot);

	items[idx++] = (SettingItem)ITEM_BUTTON_INIT(
		"Clean dot files",
		"Remove macOS junk files (.DS_Store, ._*, .Trashes, etc.)",
		dev_clean_dotfiles);

	page->title = "Developer";
	page->items = items;
	page->item_count = idx;
	page->selected = 0;
	page->scroll = 0;
	page->is_list = 0;
	page->on_show = dev_on_show;
	page->on_hide = NULL;
	page->on_tick = dev_on_tick;
	page->dynamic_start = -1;
	page->max_items = DEV_ITEM_COUNT;

	// Sync initial values
	for (int i = 0; i < idx; i++) {
		settings_item_sync(&items[i]);
	}

	return page;
}

void developer_page_destroy(SettingsPage* page) {
	if (!page)
		return;
	free(page->items);
	free(page);
}
