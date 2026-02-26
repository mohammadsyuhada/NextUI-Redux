#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <pthread.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "ui_components.h"
#include "ui_list.h"
#include "utils.h"
#include "wget_fetch.h"

// PortMaster paths
#define PORTS_PAK_DIR SDCARD_PATH "/Emus/" PLATFORM "/PORTS.pak"
#define PORTMASTER_DIR SDCARD_PATH "/Emus/shared/PortMaster"
#define PUGWASH_PATH PORTMASTER_DIR "/pugwash"
#define BASH_PATH SHARED_BIN_PATH "/bash"

// PortMaster release URL
#define PM_RELEASE_URL "https://github.com/PortsMaster/PortMaster-GUI/releases/latest/download/PortMaster.zip"
#define PM_ZIP_PATH "/tmp/PortMaster.zip"

enum PMState {
	PM_STATE_CHECK,
	PM_STATE_NOT_INSTALLED,
	PM_STATE_INSTALLED,
	PM_STATE_DOWNLOADING,
	PM_STATE_EXTRACTING,
	PM_STATE_PATCHING,
	PM_STATE_INSTALL_DONE,
	PM_STATE_INSTALL_FAILED,
	PM_STATE_NO_BASH,
	PM_STATE_LAUNCHING,
	PM_STATE_MENU,
	PM_STATE_CONFIRM_UNINSTALL,
};

static SDL_Surface* screen;
static enum PMState state = PM_STATE_CHECK;
static volatile int download_progress = 0;
static volatile bool download_cancel = false;
static volatile int download_speed = 0;
static volatile int download_eta = 0;
static volatile bool download_done = false;
static int download_result = 0;
static pthread_t download_thread;
static bool download_thread_active = false;

// Menu state
static int menu_selected = 0;
static int menu_scroll = 0;
static bool is_nintendo = true;

// Layout marker
#define LAYOUT_MARKER SHARED_USERDATA_PATH "/PORTS-portmaster/xbox_layout"

static bool is_nintendo_layout(void) {
	return access(LAYOUT_MARKER, F_OK) != 0;
}

static void toggle_layout(void);

// Menu items
#define MENU_COUNT 3
#define MENU_OPEN 0
#define MENU_LAYOUT 1
#define MENU_UNINSTALL 2

static void* download_thread_func(void* arg) {
	(void)arg;
	download_result = wget_download_file(
		PM_RELEASE_URL, PM_ZIP_PATH,
		&download_progress, &download_cancel,
		&download_speed, &download_eta);
	download_done = true;
	return NULL;
}

static bool portmaster_installed(void) {
	return access(PUGWASH_PATH, F_OK) == 0;
}

static bool bash_available(void) {
	return access(BASH_PATH, X_OK) == 0;
}

static void cleanup_portmaster(void) {
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", PORTMASTER_DIR);
	system(cmd);
}

static int extract_portmaster(void) {
	char cmd[1024];
	mkdir_p(PORTMASTER_DIR);
	snprintf(cmd, sizeof(cmd), SHARED_BIN_PATH "/busybox unzip -o '%s' -d '%s' >/dev/null 2>&1", PM_ZIP_PATH, SDCARD_PATH "/Emus/shared/");
	int ret = system(cmd);
	unlink(PM_ZIP_PATH);
	if (ret != 0) {
		cleanup_portmaster();
	}
	return ret;
}

static void patch_platform_py(void) {
	char cmd[1024];
	char platform_py[512];
	snprintf(platform_py, sizeof(platform_py), "%s/pylibs/harbourmaster/platform.py", PORTMASTER_DIR);

	// Disable portmaster_install calls in first_run (causes crash on TrimUI)
	snprintf(cmd, sizeof(cmd),
			 "sed -i 's/self\\.portmaster_install(\\[\\])/pass/g' '%s'", platform_py);
	system(cmd);
}

static void patch_device_info(void) {
	char device_info[512];
	snprintf(device_info, sizeof(device_info), "%s/device_info.txt", PORTMASTER_DIR);

	// Add TrimUI Smart Pro S (tg5050) detection before the existing TrimUI Smart Pro case
	// tg5050 uses Allwinner T527 (sun55iw3) vs tg5040's A133Plus (sun8iw20)
	char cmd[2048];
	snprintf(cmd, sizeof(cmd),
			 "if ! grep -q 'Smart Pro S' '%s' 2>/dev/null; then "
			 "sed -i '/\"trimui smart pro\"|\"trimui-smart-pro\")/i\\"
			 "    \"trimui smart pro s\"|\"trimui-smart-pro-s\")\\n"
			 "        DEVICE_CPU=\"t527\"\\n"
			 "        DEVICE_NAME=\"TrimUI Smart Pro S\"\\n"
			 "        ;;' '%s';"
			 // Make TrimUI detection distinguish tg5050 vs tg5040 by checking /proc/device-tree/model
			 "sed -i 's/DEVICE_NAME=\"TrimUI Smart Pro\"/if grep -q sun55iw3 \\/proc\\/device-tree\\/model 2>\\/dev\\/null; then DEVICE_NAME=\"TrimUI Smart Pro S\"; else DEVICE_NAME=\"TrimUI Smart Pro\"; fi/' '%s';"
			 "fi",
			 device_info, device_info, device_info);
	system(cmd);
}

static void ensure_default_config(void) {
	char config_dir[512];
	char config_path[512];
	snprintf(config_dir, sizeof(config_dir), "%s/config", PORTMASTER_DIR);
	snprintf(config_path, sizeof(config_path), "%s/config.json", config_dir);

	// Only write if config doesn't exist yet
	if (access(config_path, F_OK) == 0)
		return;

	mkdir_p(config_dir);

	FILE* fp = fopen(config_path, "w");
	if (!fp)
		return;

	fprintf(fp, "{\n");
	fprintf(fp, "    \"disclaimer\": true,\n");
	fprintf(fp, "    \"show_experimental\": false,\n");
	fprintf(fp, "    \"theme\": \"default_theme\",\n");
	fprintf(fp, "    \"theme-scheme\": \"Darkest Mode\"\n");
	fprintf(fp, "}\n");

	fclose(fp);
}

static void patch_control_txt(void) {
	char control_path[512];
	snprintf(control_path, sizeof(control_path), "%s/control.txt", PORTMASTER_DIR);

	FILE* fp = fopen(control_path, "w");
	if (!fp)
		return;

	fprintf(fp, "#!/bin/sh\n");
	fprintf(fp, "#\n");
	fprintf(fp, "# SPDX-License-Identifier: MIT\n");
	fprintf(fp, "#\n");
	fprintf(fp, "# Patched for NextUI\n");
	fprintf(fp, "\n");
	fprintf(fp, "CUR_TTY=/dev/tty0\n");
	fprintf(fp, "\n");
	fprintf(fp, "export controlfolder=\"%s\"\n", PORTMASTER_DIR);
	fprintf(fp, "export directory=\"mnt/SDCARD/.ports_temp\"\n");
	fprintf(fp, "\n");
	fprintf(fp, "PM_SCRIPTNAME=\"$(basename \"${PM_SCRIPTNAME:-$0}\")\"\n");
	fprintf(fp, "PM_PORTNAME=\"${PM_SCRIPTNAME%%.sh}\"\n");
	fprintf(fp, "\n");
	fprintf(fp, "if [ -z \"$PM_PORTNAME\" ]; then\n");
	fprintf(fp, "  PM_PORTNAME=\"Port\"\n");
	fprintf(fp, "fi\n");
	fprintf(fp, "\n");
	fprintf(fp, "export ESUDO=\"\"\n");
	fprintf(fp, "export ESUDOKILL=\"-1\"\n");
	fprintf(fp, "export SDL_GAMECONTROLLERCONFIG_FILE=\"$controlfolder/gamecontrollerdb.txt\"\n");
	fprintf(fp, "\n");
	fprintf(fp, "get_controls() {\n");
	fprintf(fp, "  sleep 0.5\n");
	fprintf(fp, "}\n");
	fprintf(fp, "\n");
	fprintf(fp, ". $controlfolder/device_info.txt\n");
	fprintf(fp, ". $controlfolder/funcs.txt\n");
	fprintf(fp, "\n");
	fprintf(fp, "export GPTOKEYB2=\"$ESUDO env LD_PRELOAD=$controlfolder/libinterpose.aarch64.so $controlfolder/gptokeyb2 $ESUDOKILL\"\n");
	fprintf(fp, "export GPTOKEYB=\"$ESUDO $controlfolder/gptokeyb $ESUDOKILL\"\n");

	fclose(fp);
}

static void fix_port_scripts(void) {
	// Fix hardcoded /roms/ports/PortMaster paths in port scripts after pugwash installs them
	char cmd[2048];
	snprintf(cmd, sizeof(cmd),
			 "export PATH=" SHARED_BIN_PATH ":$PATH && "
			 "ROM_DIR='" SDCARD_PATH "/Roms/Ports (PORTS)' && "
			 "find \"$ROM_DIR\" -maxdepth 1 -type f -name '*.sh' | while IFS= read -r f; do "
			 "if grep -q '/roms/ports/PortMaster' \"$f\" 2>/dev/null; then "
			 "sed -i 's|/roms/ports/PortMaster|" PORTMASTER_DIR "|g' \"$f\"; "
			 "fi; "
			 "if head -1 \"$f\" | grep -q '#!/bin/bash'; then "
			 "sed -i '1s|#!/bin/bash|#!/usr/bin/env bash|' \"$f\"; "
			 "fi; "
			 "done");
	system(cmd);
}

static void set_controller_layout(const char* layout) {
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "cp -f '%s/files/gamecontrollerdb_%s.txt' '%s/gamecontrollerdb.txt'",
			 PORTS_PAK_DIR, layout, PORTMASTER_DIR);
	system(cmd);
}

static void toggle_layout(void) {
	if (is_nintendo_layout()) {
		// Switch to Xbox: create marker file
		FILE* fp = fopen(LAYOUT_MARKER, "w");
		if (fp)
			fclose(fp);
		set_controller_layout("xbox");
	} else {
		// Switch to Nintendo: remove marker file
		unlink(LAYOUT_MARKER);
		set_controller_layout("nintendo");
	}
	is_nintendo = is_nintendo_layout();
}

static void launch_pugwash(void) {
	// Always ensure control.txt has correct NextUI paths before launching
	// (PortMaster's first_run may have overwritten it with default TrimUI paths)
	patch_control_txt();

	// PortMaster GUI uses Xbox button layout
	set_controller_layout("xbox");

	char cmd[2048];
	// tg5050 ships newer lib versions than what some bundled binaries expect
	system("[ ! -e " SHARED_SYSTEM_PATH "/lib/libffi.so.7 ] && [ -e /usr/lib/libffi.so.8 ] && "
		   "cp /usr/lib/libffi.so.8 " SHARED_SYSTEM_PATH "/lib/libffi.so.7;"
		   "[ ! -e " SHARED_SYSTEM_PATH "/lib/libncurses.so.5 ] && [ -e /usr/lib/libncurses.so.6 ] && "
		   "cp /usr/lib/libncurses.so.6 " SHARED_SYSTEM_PATH "/lib/libncurses.so.5");

	snprintf(cmd, sizeof(cmd),
			 "export LD_LIBRARY_PATH=" SYSTEM_PATH "/lib:" SHARED_SYSTEM_PATH "/lib:/usr/trimui/lib:/usr/lib:$LD_LIBRARY_PATH && "
			 "export PATH=" SYSTEM_PATH "/bin:" SHARED_BIN_PATH ":/usr/trimui/bin:$PATH && "
			 "export PYSDL2_DLL_PATH=/usr/trimui/lib:/usr/lib && "
			 "export SSL_CERT_FILE=" SHARED_SYSTEM_PATH "/etc/ssl/certs/ca-certificates.crt && "
			 "export HOME=" SHARED_USERDATA_PATH "/PORTS-portmaster && "
			 "export XDG_DATA_HOME=$HOME/.local/share && "
			 "mkdir -p $XDG_DATA_HOME && "
			 "mkdir -p $XDG_DATA_HOME/PortMaster && "
			 "mount -o bind '" PORTMASTER_DIR "' $XDG_DATA_HOME/PortMaster 2>/dev/null; "
			 "export HM_TOOLS_DIR='" SDCARD_PATH "/Emus/shared' && "
			 "export HM_PORTS_DIR='" SDCARD_PATH "/Roms/Ports (PORTS)/.ports' && "
			 "export HM_SCRIPTS_DIR='" SDCARD_PATH "/Roms/Ports (PORTS)' && "
			 "export SDL_GAMECONTROLLERCONFIG_FILE='%s/gamecontrollerdb.txt' && "
			 "cd '%s' && "
			 "rm -f .pugwash-reboot && "
			 "while true; do " SHARED_BIN_PATH "/python3 pugwash --debug 2>&1 | tee " SDCARD_PATH "/.userdata/" PLATFORM "/logs/portmaster_pugwash.txt; "
			 "[ ! -f .pugwash-reboot ] && break; "
			 "rm -f .pugwash-reboot; "
			 "done",
			 PORTMASTER_DIR, PORTMASTER_DIR);
	system(cmd);

	// Re-patch control.txt after pugwash exits
	// (pugwash first_run or updates may have overwritten it)
	patch_control_txt();

	// Fix hardcoded paths in any newly installed port scripts
	fix_port_scripts();
}

static void format_speed(int bps, char* buf, int buf_size) {
	if (bps >= 1048576)
		snprintf(buf, buf_size, "%.1f MB/s", bps / 1048576.0);
	else if (bps >= 1024)
		snprintf(buf, buf_size, "%d KB/s", bps / 1024);
	else
		snprintf(buf, buf_size, "%d B/s", bps);
}

static void start_download(void) {
	state = PM_STATE_DOWNLOADING;
	download_progress = 0;
	download_cancel = false;
	download_speed = 0;
	download_eta = 0;
	download_result = 0;
	download_done = false;
	pthread_create(&download_thread, NULL, download_thread_func, NULL);
	download_thread_active = true;
}

static void render_screen(void) {
	GFX_clear(screen);

	switch (state) {
	case PM_STATE_CHECK:
		UI_renderMenuBar(screen, "PortMaster");
		UI_renderCenteredMessage(screen, "Checking installation...");
		break;

	case PM_STATE_NOT_INSTALLED:
		UI_renderMenuBar(screen, "PortMaster");
		{
			char* lines = "PortMaster is not installed.\nPress A to download and install.";
			int y = screen->h / 2 - SCALE1(FONT_LARGE);
			GFX_blitText(font.large, lines, SCALE1(4), COLOR_WHITE, screen,
						 &(SDL_Rect){SCALE1(PADDING), y, screen->w - SCALE1(PADDING * 2), screen->h});
		}
		UI_renderButtonHintBar(screen, (char*[]){"A", "INSTALL", "B", "BACK", NULL});
		break;

	case PM_STATE_DOWNLOADING:
		UI_renderMenuBar(screen, "PortMaster");
		{
			char status[256];
			char speed_str[64];
			format_speed(download_speed, speed_str, sizeof(speed_str));

			if (download_eta > 0)
				snprintf(status, sizeof(status), "Downloading... %d%%  (%s, %ds left)", download_progress, speed_str, download_eta);
			else
				snprintf(status, sizeof(status), "Downloading... %d%%  (%s)", download_progress, speed_str);

			int bar_x = SCALE1(PADDING * 3);
			int bar_w = screen->w - SCALE1(PADDING * 6);
			int bar_h = SCALE1(12);
			int bar_y = screen->h / 2;

			GFX_blitText(font.medium, status, 0, COLOR_WHITE, screen,
						 &(SDL_Rect){bar_x, bar_y - SCALE1(FONT_MEDIUM + 8), bar_w, SCALE1(FONT_MEDIUM)});

			SDL_Rect bg_rect = {bar_x, bar_y, bar_w, bar_h};
			SDL_FillRect(screen, &bg_rect, SDL_MapRGB(screen->format, 0x26, 0x26, 0x26));

			int fill_w = (bar_w * download_progress) / 100;
			if (fill_w > 0) {
				SDL_Rect fill_rect = {bar_x, bar_y, fill_w, bar_h};
				SDL_FillRect(screen, &fill_rect, SDL_MapRGB(screen->format, 0xff, 0xff, 0xff));
			}
		}
		UI_renderButtonHintBar(screen, (char*[]){"B", "CANCEL", NULL});
		break;

	case PM_STATE_EXTRACTING:
		UI_renderMenuBar(screen, "PortMaster");
		UI_renderCenteredMessage(screen, "Extracting PortMaster...");
		break;

	case PM_STATE_PATCHING:
		UI_renderMenuBar(screen, "PortMaster");
		UI_renderCenteredMessage(screen, "Configuring for NextUI...");
		break;

	case PM_STATE_INSTALL_DONE:
		UI_renderMenuBar(screen, "PortMaster");
		UI_renderCenteredMessage(screen, "Installation complete!");
		UI_renderButtonHintBar(screen, (char*[]){"A", "LAUNCH", "B", "BACK", NULL});
		break;

	case PM_STATE_INSTALL_FAILED:
		UI_renderMenuBar(screen, "PortMaster");
		UI_renderCenteredMessage(screen, "Installation failed. Check WiFi and try again.");
		UI_renderButtonHintBar(screen, (char*[]){"A", "RETRY", "B", "BACK", NULL});
		break;

	case PM_STATE_NO_BASH:
		UI_renderMenuBar(screen, "PortMaster");
		UI_renderCenteredMessage(screen, "Error: bash not found. PortMaster requires bash.");
		UI_renderButtonHintBar(screen, (char*[]){"B", "BACK", NULL});
		break;

	case PM_STATE_INSTALLED:
	case PM_STATE_LAUNCHING:
		UI_renderMenuBar(screen, "PortMaster");
		UI_renderCenteredMessage(screen, "Launching PortMaster...");
		break;

	case PM_STATE_MENU: {
		UI_renderMenuBar(screen, "PortMaster");
		ListLayout layout = UI_calcListLayout(screen);

		UISettingsItem items[] = {
			{.label = "Open PortMaster", .swatch = -1, .desc = "Launch the PortMaster GUI"},
			{.label = "Button Layout", .value = is_nintendo ? "Nintendo" : "Xbox", .swatch = -1, .cycleable = 1, .desc = "Button layout for in-game port controls"},
			{.label = "Uninstall PortMaster", .swatch = -1, .desc = "Remove PortMaster from your device"},
		};

		UI_renderSettingsPage(screen, &layout, items, MENU_COUNT,
							  menu_selected, &menu_scroll, NULL);

		bool is_layout = (menu_selected == MENU_LAYOUT);
		UI_renderButtonHintBar(screen, (char*[]){
										   "B", "EXIT",
										   is_layout ? "LEFT/RIGHT" : "A",
										   is_layout ? "CHANGE" : "OPEN",
										   NULL});
		break;
	}

	case PM_STATE_CONFIRM_UNINSTALL:
		UI_renderConfirmDialog(screen, "Uninstall PortMaster?", "This cannot be undone.");
		break;
	}

	GFX_flip(screen);
}

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	screen = GFX_init(MODE_MAIN);
	UI_showSplashScreen(screen, "PortMaster");

	InitSettings();
	PAD_init();
	PWR_init();
	setup_signal_handlers();

	// Check bash availability
	if (!bash_available()) {
		state = PM_STATE_NO_BASH;
		bool dirty = true;
		IndicatorType show_setting = INDICATOR_NONE;
		while (!app_quit) {
			GFX_startFrame();
			PAD_poll();
			PWR_update(&dirty, &show_setting, NULL, NULL);
			if (PAD_justPressed(BTN_B))
				app_quit = true;
			if (dirty) {
				render_screen();
				dirty = false;
			} else {
				GFX_sync();
			}
		}
		QuitSettings();
		PWR_quit();
		PAD_quit();
		GFX_quit();
		return EXIT_FAILURE;
	}

	// Start in menu if installed, otherwise show install screen
	if (portmaster_installed()) {
		ensure_default_config();
		is_nintendo = is_nintendo_layout();
		state = PM_STATE_MENU;
	} else {
		state = PM_STATE_NOT_INSTALLED;
	}

	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (!app_quit) {
		GFX_startFrame();
		PAD_poll();
		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (UI_statusBarChanged())
			dirty = true;

		switch (state) {
		case PM_STATE_MENU:
			if (PAD_navigateMenu(&menu_selected, MENU_COUNT))
				dirty = true;
			if (menu_selected == MENU_LAYOUT &&
				(PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT))) {
				toggle_layout();
				dirty = true;
			}
			if (PAD_justPressed(BTN_A)) {
				switch (menu_selected) {
				case MENU_OPEN:
					state = PM_STATE_LAUNCHING;
					dirty = true;
					break;
				case MENU_LAYOUT:
					toggle_layout();
					dirty = true;
					break;
				case MENU_UNINSTALL:
					state = PM_STATE_CONFIRM_UNINSTALL;
					dirty = true;
					break;
				}
			}
			if (PAD_justPressed(BTN_B))
				app_quit = true;
			break;

		case PM_STATE_CONFIRM_UNINSTALL:
			if (PAD_justPressed(BTN_A)) {
				cleanup_portmaster();
				state = PM_STATE_NOT_INSTALLED;
				dirty = true;
			} else if (PAD_justPressed(BTN_B)) {
				state = PM_STATE_MENU;
				dirty = true;
			}
			break;

		case PM_STATE_NOT_INSTALLED:
			if (PAD_justPressed(BTN_A)) {
				start_download();
				dirty = true;
			} else if (PAD_justPressed(BTN_B)) {
				app_quit = true;
			}
			break;

		case PM_STATE_DOWNLOADING:
			dirty = true; // always redraw for progress
			if (PAD_justPressed(BTN_B)) {
				download_cancel = true;
				if (download_thread_active) {
					pthread_join(download_thread, NULL);
					download_thread_active = false;
				}
				download_done = false;
				unlink(PM_ZIP_PATH);
				state = PM_STATE_NOT_INSTALLED;
			}
			// Check if download thread finished (success or failure)
			if (download_thread_active && download_done) {
				pthread_join(download_thread, NULL);
				download_thread_active = false;
				download_done = false;
				if (download_result > 0) {
					state = PM_STATE_EXTRACTING;
				} else {
					state = PM_STATE_INSTALL_FAILED;
				}
			}
			break;

		case PM_STATE_EXTRACTING:
			render_screen();
			{
				int ret = extract_portmaster();
				if (ret == 0 && portmaster_installed()) {
					state = PM_STATE_PATCHING;
				} else {
					cleanup_portmaster();
					state = PM_STATE_INSTALL_FAILED;
				}
			}
			dirty = true;
			break;

		case PM_STATE_PATCHING:
			render_screen();
			patch_control_txt();
			patch_platform_py();
			patch_device_info();
			ensure_default_config();
			{
				char cmd[512];
				snprintf(cmd, sizeof(cmd), "chmod -R +x '%s' 2>/dev/null", PORTMASTER_DIR);
				system(cmd);
			}
			state = PM_STATE_INSTALL_DONE;
			dirty = true;
			break;

		case PM_STATE_INSTALL_DONE:
			if (PAD_justPressed(BTN_A)) {
				state = PM_STATE_LAUNCHING;
				dirty = true;
			} else if (PAD_justPressed(BTN_B)) {
				app_quit = true;
			}
			break;

		case PM_STATE_INSTALL_FAILED:
			if (PAD_justPressed(BTN_A)) {
				start_download();
				dirty = true;
			} else if (PAD_justPressed(BTN_B)) {
				app_quit = true;
			}
			break;

		case PM_STATE_LAUNCHING:
			render_screen();
			QuitSettings();
			PWR_quit();
			PAD_quit();
			GFX_quit();

			launch_pugwash();

			// Re-init SDL after pugwash returns
			screen = GFX_init(MODE_MAIN);
			InitSettings();
			PAD_init();
			PWR_init();

			// Check if user uninstalled from within pugwash
			if (portmaster_installed()) {
				is_nintendo = is_nintendo_layout();
				state = PM_STATE_MENU;
			} else {
				state = PM_STATE_NOT_INSTALLED;
			}
			dirty = true;
			break;

		default:
			break;
		}

		if (dirty) {
			render_screen();
			dirty = false;
		} else {
			GFX_sync();
		}
	}

	if (download_thread_active) {
		download_cancel = true;
		pthread_join(download_thread, NULL);
	}

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
