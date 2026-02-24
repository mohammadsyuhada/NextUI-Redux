#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <msettings.h>

#include "psa/crypto.h"
#include "api.h"
#include "player.h"

// UI modules
#include "ui_icons.h"
#include "ui_components.h"

// Module architecture
#include "module_common.h"
#include "module_menu.h"
#include "module_library.h"
#include "module_player.h"
#include "module_radio.h"
#include "module_podcast.h"
#include "downloader.h"
#include "module_settings.h"
#include "settings.h"
#include "resume.h"
#include "background.h"
#include "display_helper.h"

// Global quit flag
static bool quit = false;
static SDL_Surface* screen;

static void sigHandler(int sig) {
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		quit = true;
		break;
	default:
		break;
	}
}

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	screen = GFX_init(MODE_MAIN);
	UI_showSplashScreen(screen, "Music Player");

	InitSettings();
	PAD_init();
	PWR_init();
	WIFI_init();
	psa_crypto_init();
	Icons_init();

	signal(SIGINT, sigHandler);
	signal(SIGTERM, sigHandler);

	// Seed random number generator for shuffle
	srand((unsigned int)time(NULL));

	// Mute hardware before opening audio device to prevent amplifier pop on TG5050
	SetRawVolume(0);

	// Initialize player core
	if (Player_init() != 0) {
		LOG_error("Failed to initialize audio player\n");
		goto cleanup;
	}

	// At startup, set software volume based on output device
	if (Player_isBluetoothActive() || Player_isUSBDACActive()) {
		// Use cubic curve for perceptual volume (human hearing is logarithmic)
		float v = GetVolume() / 20.0f;
		Player_setVolume(v * v * v);
	} else {
		Player_setVolume(1.0f);
	}

	// Restore hardware volume after audio device is open and stable
	SetVolume(GetVolume());

	// Initialize common module (global input handling)
	ModuleCommon_init();

	// Initialize app-specific settings
	Settings_init();

	// Initialize resume state
	Resume_init();

	// Initialize YouTube downloader (loads queue, auto-resumes pending downloads)
	Downloader_init();

	// Main application loop
	while (!quit) {
		// Run main menu - returns selected item or MENU_QUIT
		int selection = MenuModule_run(screen);

		if (selection == MENU_QUIT) {
			quit = true;
			continue;
		}

		// Run the selected module
		ModuleExitReason reason = MODULE_EXIT_TO_MENU;

		switch (selection) {
		case MENU_RESUME: { // Also MENU_NOW_PLAYING (same slot)
			if (Background_isPlaying()) {
				// "Now Playing" — route to the active background module
				switch (Background_getActive()) {
				case BG_MUSIC:
					reason = PlayerModule_run(screen);
					break;
				case BG_RADIO:
					reason = RadioModule_run(screen);
					break;
				case BG_PODCAST:
					reason = PodcastModule_run(screen);
					break;
				default:
					break;
				}
			} else {
				// "Resume" — load saved state
				const ResumeState* rs = Resume_getState();
				if (rs) {
					reason = PlayerModule_runResume(screen, rs);
				}
			}
			break;
		}
		case MENU_LIBRARY:
			reason = LibraryModule_run(screen);
			break;
		case MENU_RADIO:
			reason = RadioModule_run(screen);
			break;
		case MENU_PODCAST:
			reason = PodcastModule_run(screen);
			break;
		case MENU_SETTINGS:
			reason = SettingsModule_run(screen);
			break;
		}

		// TG5050: modules may have triggered display recovery (new screen surface)
		{
			SDL_Surface* ns = DisplayHelper_getReinitScreen();
			if (ns)
				screen = ns;
		}

		if (reason == MODULE_EXIT_QUIT) {
			quit = true;
		}
	}

cleanup:
	GFX_clear(screen);
	GFX_flip(screen);

	Background_stopAll();
	Downloader_cleanup();
	Settings_quit();
	ModuleCommon_quit();
	Player_quit();
	Icons_quit();

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
