#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <msettings.h>

#include "api.h"

#include "ui_icons.h"

#include "module_common.h"
#include "module_menu.h"
#include "module_player.h"
#include "module_youtube.h"
#include "module_iptv.h"
#include "module_settings.h"
#include "ffplay_engine.h"
#include "settings.h"
#include "ytdlp_updater.h"
#include "youtube.h"
#include "subscriptions.h"
#include "iptv.h"
#include "iptv_curated.h"
#include "ui_keyboard.h"

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
	// Show splash screen immediately while subsystems initialize
	{
		GFX_clear(screen);
		SDL_Surface* title = TTF_RenderUTF8_Blended(font.title, "Media Player", COLOR_WHITE);
		if (title) {
			SDL_BlitSurface(title, NULL, screen, &(SDL_Rect){(screen->w - title->w) / 2, screen->h / 2 - title->h});
			SDL_FreeSurface(title);
		}
		SDL_Surface* loading = TTF_RenderUTF8_Blended(font.small, "Loading...", COLOR_GRAY);
		if (loading) {
			SDL_BlitSurface(loading, NULL, screen, &(SDL_Rect){(screen->w - loading->w) / 2, screen->h / 2 + SCALE1(4)});
			SDL_FreeSurface(loading);
		}
		GFX_flip(screen);
	}

	InitSettings();

	// TG5050: warm up audio codec while muted to prevent amplifier pop on first playback.
	// After reboot the codec is powered off — mixer writes don't reach the analog output
	// until the codec powers on, so the first PCM open would pop without this.
	if (strcmp(PLATFORM, "tg5050") == 0) {
		SetRawVolume(0);
		SDL_InitSubSystem(SDL_INIT_AUDIO);
		SDL_AudioSpec want = {0};
		want.freq = 44100;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 1024;
		SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
		if (dev > 0)
			SDL_CloseAudioDevice(dev);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		SetVolume(GetVolume());
	}

	PAD_init();
	PWR_init();
	// No WIFI_init at startup - WiFi enabled on demand
	Icons_init();

	signal(SIGINT, sigHandler);
	signal(SIGTERM, sigHandler);

	// Initialize common module (global input handling)
	ModuleCommon_init();

	// Initialize app-specific settings
	Settings_init();

	// Initialize yt-dlp updater
	YtdlpUpdater_init();

	// Initialize YouTube module
	YouTube_init();
	UIKeyboard_init();

	// Initialize subscriptions (loads from disk)
	Subscriptions_init();

	// Initialize IPTV (loads playlists + cached channels)
	IPTV_init();
	IPTV_curated_init();

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
		case MENU_LOCAL:
			reason = PlayerModule_run(screen);
			break;
		case MENU_YOUTUBE:
			reason = YouTubeModule_run(screen);
			break;
		case MENU_IPTV:
			reason = IPTVModule_run(screen);
			break;
		case MENU_SETTINGS:
			reason = SettingsModule_run(screen);
			break;
		}

		// TG5050: modules may have triggered display recovery (new screen surface)
		{
			SDL_Surface* ns = FfplayEngine_getReinitScreen();
			if (ns)
				screen = ns;
		}

		// Re-enable autosleep when returning to main menu
		ModuleCommon_setAutosleepDisabled(false);

		if (reason == MODULE_EXIT_QUIT) {
			quit = true;
		}
	}

	IPTV_curated_cleanup();
	IPTV_cleanup();
	Subscriptions_cleanup();
	YouTube_cleanup();
	YtdlpUpdater_cleanup();
	Settings_quit();
	ModuleCommon_quit();
	Icons_quit();

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
