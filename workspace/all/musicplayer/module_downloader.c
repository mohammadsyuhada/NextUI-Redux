#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "module_downloader.h"
#include "display_helper.h"
#include "module_library.h"
#include "downloader.h"
#include "ui_downloader.h"
#include "wifi.h"

// Menu count
#define DOWNLOADER_MENU_COUNT 2

// Internal states
typedef enum {
	DOWNLOADER_INTERNAL_MENU,
	DOWNLOADER_INTERNAL_SEARCHING,
	DOWNLOADER_INTERNAL_RESULTS,
	DOWNLOADER_INTERNAL_QUEUE
} DownloaderInternalState;

// Module state
static int menu_selected = 0;
static int results_selected = 0;
static int results_scroll = 0;
static int queue_selected = 0;
static int queue_scroll = 0;
static DownloaderResult* results = NULL;
static int result_count = 0;
static char toast_message[128] = "";
static uint32_t toast_time = 0;

ModuleExitReason DownloaderModule_run(SDL_Surface* screen) {
	Downloader_init();

	// Check WiFi before entering
	IndicatorType show_setting = INDICATOR_NONE;
	if (!Downloader_isAvailable()) {
		Downloader_cleanup();
		LibraryModule_setToast("Downloader not available");
		return MODULE_EXIT_TO_MENU;
	}
	if (!Wifi_ensureConnected(screen, show_setting)) {
		Downloader_cleanup();
		LibraryModule_setToast("Internet connection required");
		return MODULE_EXIT_TO_MENU;
	}

	DownloaderInternalState state = DOWNLOADER_INTERNAL_MENU;
	bool dirty = true;
	char search_query[256] = "";

	menu_selected = 0;
	results_selected = 0;
	results_scroll = 0;
	queue_selected = 0;
	queue_scroll = 0;
	results = NULL;
	result_count = 0;
	toast_message[0] = '\0';

	// If re-entering while download is running, go straight to queue
	if (Downloader_isDownloading()) {
		queue_selected = 0;
		queue_scroll = 0;
		state = DOWNLOADER_INTERNAL_QUEUE;
	}

	while (1) {
		GFX_startFrame();
		PAD_poll();

		// Handle global input
		int app_state_for_help;
		switch (state) {
		case DOWNLOADER_INTERNAL_MENU:
			app_state_for_help = 28;
			break;
		case DOWNLOADER_INTERNAL_SEARCHING:
			app_state_for_help = 29;
			break;
		case DOWNLOADER_INTERNAL_RESULTS:
			app_state_for_help = 30;
			break;
		case DOWNLOADER_INTERNAL_QUEUE:
			app_state_for_help = 31;
			break;
		}

		GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, app_state_for_help);
		if (global.should_quit) {
			Downloader_cleanup();
			return MODULE_EXIT_QUIT;
		}
		if (global.input_consumed) {
			if (global.dirty)
				dirty = 1;
			GFX_sync();
			continue;
		}

		// =========================================
		// MENU STATE
		// =========================================
		if (state == DOWNLOADER_INTERNAL_MENU) {
			if (PAD_justRepeated(BTN_UP)) {
				menu_selected = (menu_selected > 0) ? menu_selected - 1 : DOWNLOADER_MENU_COUNT - 1;
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN)) {
				menu_selected = (menu_selected < DOWNLOADER_MENU_COUNT - 1) ? menu_selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A)) {
				if (menu_selected == 0) {
					// Search Music
					char* query = Downloader_openKeyboard("Search:");
					PAD_reset();
					PAD_poll();
					PAD_reset();
					// TG5050: display recovery creates a new screen surface
					{
						SDL_Surface* ns = DisplayHelper_getReinitScreen();
						if (ns)
							screen = ns;
					}
					if (query && strlen(query) > 0) {
						snprintf(search_query, sizeof(search_query), "%s", query);
						results_scroll = 0;
						results = NULL;
						result_count = 0;
						if (Downloader_startSearch(query) == 0) {
							state = DOWNLOADER_INTERNAL_SEARCHING;
						} else {
							// Search failed to start (likely another search in progress)
							snprintf(toast_message, sizeof(toast_message), "Search already in progress");
							toast_time = SDL_GetTicks();
						}
					}
					if (query)
						free(query);
					dirty = 1;
				} else if (menu_selected == 1) {
					// Download Queue
					queue_selected = 0;
					queue_scroll = 0;
					state = DOWNLOADER_INTERNAL_QUEUE;
					dirty = 1;
				}
			} else if (PAD_justPressed(BTN_B)) {
				if (Downloader_isDownloading()) {
					// Keep download running in background
					Downloader_saveQueue();
				} else {
					Downloader_cleanup();
				}
				return MODULE_EXIT_TO_MENU;
			}
		}
		// =========================================
		// SEARCHING STATE
		// =========================================
		else if (state == DOWNLOADER_INTERNAL_SEARCHING) {
			Downloader_update();
			const DownloaderSearchStatus* search_status = Downloader_getSearchStatus();
			if (search_status->completed) {
				if (search_status->result_count > 0) {
					results = Downloader_getSearchResults();
					result_count = search_status->result_count;
					results_selected = -1;
					state = DOWNLOADER_INTERNAL_RESULTS;
				} else {
					snprintf(toast_message, sizeof(toast_message), "%s",
							 search_status->error_message[0] ? search_status->error_message : "No results found");
					toast_time = SDL_GetTicks();
					state = DOWNLOADER_INTERNAL_MENU;
				}
			}

			if (PAD_justPressed(BTN_B)) {
				Downloader_cancelSearch();
				state = DOWNLOADER_INTERNAL_MENU;
			}

			dirty = 1; // Keep refreshing for spinner
		}
		// =========================================
		// RESULTS STATE
		// =========================================
		else if (state == DOWNLOADER_INTERNAL_RESULTS) {
			if (PAD_justRepeated(BTN_UP) && result_count > 0) {
				if (results_selected < 0) {
					results_selected = result_count - 1;
				} else {
					results_selected = (results_selected > 0) ? results_selected - 1 : result_count - 1;
				}
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN) && result_count > 0) {
				if (results_selected < 0) {
					results_selected = 0;
				} else {
					results_selected = (results_selected < result_count - 1) ? results_selected + 1 : 0;
				}
				dirty = 1;
			} else if (PAD_justPressed(BTN_A) && result_count > 0 && results_selected >= 0) {
				DownloaderResult* r = &results[results_selected];
				if (Downloader_isInQueue(r->video_id)) {
					snprintf(toast_message, sizeof(toast_message), "Already in queue");
				} else {
					int added = Downloader_queueAdd(r->video_id, r->title);
					if (added == 1) {
						// queueAdd auto-starts download thread
						if (Downloader_isDownloading()) {
							snprintf(toast_message, sizeof(toast_message), "Added to queue");
						} else {
							snprintf(toast_message, sizeof(toast_message), "Downloading...");
						}
					} else if (added == -1) {
						snprintf(toast_message, sizeof(toast_message), "Queue is full");
					}
				}
				toast_time = SDL_GetTicks();
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				toast_message[0] = '\0';
				downloader_results_clear_scroll();
				GFX_clearLayers(LAYER_SCROLLTEXT);
				state = DOWNLOADER_INTERNAL_MENU;
				dirty = 1;
			}

			if (downloader_results_needs_scroll_refresh()) {
				downloader_results_animate_scroll();
			}
			if (downloader_results_scroll_needs_render())
				dirty = 1;
		}
		// =========================================
		// QUEUE STATE
		// =========================================
		else if (state == DOWNLOADER_INTERNAL_QUEUE) {
			int qcount = Downloader_queueCount();

			if (PAD_justRepeated(BTN_UP) && qcount > 0) {
				queue_selected = (queue_selected > 0) ? queue_selected - 1 : qcount - 1;
				dirty = 1;
			} else if (PAD_justRepeated(BTN_DOWN) && qcount > 0) {
				queue_selected = (queue_selected < qcount - 1) ? queue_selected + 1 : 0;
				dirty = 1;
			} else if (PAD_justPressed(BTN_A) && qcount > 0) {
				// Queue is now a monitoring page — downloads auto-start from search
			} else if (PAD_justPressed(BTN_X) && qcount > 0) {
				Downloader_queueRemove(queue_selected);
				downloader_queue_clear_scroll();
				if (queue_selected >= Downloader_queueCount() && queue_selected > 0) {
					queue_selected--;
				}
				dirty = 1;
			} else if (PAD_justPressed(BTN_B)) {
				downloader_queue_clear_scroll();
				state = DOWNLOADER_INTERNAL_MENU;
				dirty = 1;
			}

			if (downloader_queue_needs_scroll_refresh()) {
				downloader_queue_animate_scroll();
			}
			if (downloader_queue_scroll_needs_render())
				dirty = 1;
		}
		// Keep refreshing while download is active (for progress updates)
		if (state == DOWNLOADER_INTERNAL_QUEUE && Downloader_isDownloading()) {
			dirty = 1;
		}
		// Handle power management
		ModuleCommon_PWR_update(&dirty, &show_setting);

		// Render
		if (dirty) {
			switch (state) {
			case DOWNLOADER_INTERNAL_MENU:
				render_downloader_menu(screen, show_setting, menu_selected, toast_message, toast_time);
				break;
			case DOWNLOADER_INTERNAL_SEARCHING:
				render_downloader_searching(screen, show_setting, search_query);
				break;
			case DOWNLOADER_INTERNAL_RESULTS:
				render_downloader_results(screen, show_setting, search_query, results, result_count,
										  results_selected, &results_scroll, toast_message, toast_time, false);
				break;
			case DOWNLOADER_INTERNAL_QUEUE:
				render_downloader_queue(screen, show_setting, queue_selected, &queue_scroll);
				break;
			}

			GFX_flip(screen);
			dirty = 0;

			// Toast refresh
			ModuleCommon_tickToast(toast_message, toast_time, &dirty);
		} else {
			GFX_sync();
		}
	}
}
