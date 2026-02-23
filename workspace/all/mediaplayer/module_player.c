#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "vp_defines.h"
#include "api.h"
#include "module_common.h"
#include "module_player.h"
#include "video_browser.h"
#include "ffplay_engine.h"
#include "ui_player.h"

// Browser scroll text state (for selected item marquee)
static ScrollTextState browser_scroll = {0};

// Helper: load directory into browser context
static void load_video_directory(VideoBrowserContext* browser, const char* path) {
	VideoBrowser_loadDirectory(browser, path, VIDEO_ROOT);
}

ModuleExitReason PlayerModule_run(SDL_Surface* screen) {
	VideoBrowserContext browser;
	memset(&browser, 0, sizeof(browser));

	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	// Create video root if needed and load initial directory
	mkdir(VIDEO_ROOT, 0755);
	load_video_directory(&browser, VIDEO_ROOT);

	// Reset browser scroll state
	memset(&browser_scroll, 0, sizeof(browser_scroll));

	while (1) {
		PAD_poll();

		// Handle global input (START dialogs, volume, etc.)
		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, STATE_BROWSER);
		if (global.should_quit) {
			VideoBrowser_freeEntries(&browser);
			return MODULE_EXIT_QUIT;
		}
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}

		// Browser navigation
		if (PAD_justPressed(BTN_B)) {
			// Go up or exit to menu
			if (strcmp(browser.current_path, VIDEO_ROOT) != 0) {
				// Navigate to parent directory
				char* last_slash = strrchr(browser.current_path, '/');
				if (last_slash && last_slash != browser.current_path) {
					*last_slash = '\0';
					load_video_directory(&browser, browser.current_path);
				} else {
					load_video_directory(&browser, VIDEO_ROOT);
				}
				GFX_clearLayers(LAYER_SCROLLTEXT);
				dirty = 1;
			} else {
				// At root, return to main menu
				GFX_clearLayers(LAYER_SCROLLTEXT);
				VideoBrowser_freeEntries(&browser);
				return MODULE_EXIT_TO_MENU;
			}
		} else if (browser.entry_count > 0) {
			if (PAD_justRepeated(BTN_UP)) {
				browser.selected = (browser.selected > 0) ? browser.selected - 1 : browser.entry_count - 1;
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN)) {
				browser.selected = (browser.selected < browser.entry_count - 1) ? browser.selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				VideoFileEntry* entry = &browser.entries[browser.selected];

				if (entry->is_dir) {
					// Open directory (handle ".." parent entry too)
					char path_copy[512];
					snprintf(path_copy, sizeof(path_copy), "%s", entry->path);
					load_video_directory(&browser, path_copy);
					GFX_clearLayers(LAYER_SCROLLTEXT);
					dirty = 1;
				} else {
					// Play video file via ffplay
					FfplayConfig config;
					memset(&config, 0, sizeof(config));
					config.source = FFPLAY_SOURCE_LOCAL;
					config.is_stream = false;
					config.start_position_sec = 0;
					config.screen_width = screen->w;
					config.is_hevc = VideoBrowser_isHEVC(entry->path);
					strncpy(config.path, entry->path, sizeof(config.path) - 1);
					config.path[sizeof(config.path) - 1] = '\0';
					VideoBrowser_getDisplayName(entry->name, config.title, sizeof(config.title));

					// Subtitle handling:
					// 1. Multiple external files (.srt/.ass next to video) — always preferred
					//    D-pad DOWN cycles through them + an "off" state
					// 2. Embedded in video — uses video path as subtitle source,
					//    but only for files under 500MB (dual-demux blocks on large files)
					//    Skipped for HEVC — subtitle overlay + HEVC decode is too CPU-heavy
					SubtitleList sub_list;
					VideoBrowser_findSubtitles(entry->path, &sub_list);

					if (sub_list.count > 0) {
						config.subtitle_count = sub_list.count;
						for (int si = 0; si < sub_list.count; si++) {
							strncpy(config.subtitle_paths[si], sub_list.entries[si].path, sizeof(config.subtitle_paths[0]) - 1);
							config.subtitle_paths[si][sizeof(config.subtitle_paths[0]) - 1] = '\0';
							strncpy(config.subtitle_labels[si], sub_list.entries[si].label, sizeof(config.subtitle_labels[0]) - 1);
							config.subtitle_labels[si][sizeof(config.subtitle_labels[0]) - 1] = '\0';
						}
						// Also set legacy fields to the first subtitle for compatibility
						strncpy(config.subtitle_path, sub_list.entries[0].path, sizeof(config.subtitle_path) - 1);
						config.subtitle_path[sizeof(config.subtitle_path) - 1] = '\0';
						config.subtitle_is_external = true;
					} else if (!config.is_hevc) {
						// Embedded subs: only for non-HEVC files under 500MB.
						// HEVC decode already saturates the CPU — adding the subtitle
						// overlay filter on top causes frame drops or audio-only playback.
						struct stat vst;
						if (stat(entry->path, &vst) == 0 && vst.st_size < (off_t)500 * 1024 * 1024) {
							strncpy(config.subtitle_path, entry->path, sizeof(config.subtitle_path) - 1);
							config.subtitle_path[sizeof(config.subtitle_path) - 1] = '\0';
							config.subtitle_is_external = false;
						}
					}

					// Disable autosleep during playback
					ModuleCommon_setAutosleepDisabled(true);

					// Launch ffplay (releases PAD, waits, re-inits PAD)
					FfplayEngine_play(&config);

					// Reset scroll state and force full redraw
					memset(&browser_scroll, 0, sizeof(browser_scroll));
					dirty = 1;
				}
			}
		}

		// Animate browser scroll text (GPU mode)
		if (ScrollText_isScrolling(&browser_scroll)) {
			ScrollText_animateOnly(&browser_scroll);
		}
		if (ScrollText_needsRender(&browser_scroll)) {
			dirty = 1;
		}

		// Handle power management
		ModuleCommon_PWR_update(&dirty, &show_setting);

		// Render
		if (dirty) {
			render_video_browser(screen, show_setting, &browser, &browser_scroll);

			GFX_flip(screen);
			dirty = 0;
		} else {
			GFX_sync();
		}
	}
}
